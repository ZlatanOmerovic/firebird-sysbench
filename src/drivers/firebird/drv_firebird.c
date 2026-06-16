/* Copyright (C) 2026 Zlatan Omerovic

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_STRING_H
# include <string.h>
#endif
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <dlfcn.h>

#include "firebird/fb_c_api.h"

#include "sb_options.h"
#include "db_driver.h"

#define xfree(ptr) ({ if (ptr) free((void *)ptr); ptr = NULL; })

#define MAX_COLUMN_LENGTH 512UL
#define FB_DIALECT 3

static sb_arg_t firebird_drv_args[] =
{
  SB_OPT("firebird-db", "Firebird database connection string",
         "localhost:/tmp/sbtest.fdb", STRING),
  SB_OPT("firebird-user", "Firebird user", "SYSDBA", STRING),
  SB_OPT("firebird-password", "Firebird password", "masterkey", STRING),
  SB_OPT("firebird-client",
         "Path to libfbclient.so to load at runtime (overrides default)",
         "libfbclient.so", STRING),

  SB_OPT_END
};

typedef struct
{
  const char *db;
  const char *user;
  const char *password;
  const char *client;
} fb_drv_args_t;

/* Per-process globals — initialized once in drv_init */
static struct IMaster *fb_master;
static struct IProvider *fb_prov;
static struct IUtil *fb_utl;

/* libfbclient.so handles resolved at runtime via dlopen/dlsym. */
static void *fb_dlhandle;
typedef struct IMaster *(*fb_get_master_t)(void);
typedef ISC_LONG (*isc_sqlcode_t)(const ISC_STATUS *);
static fb_get_master_t p_fb_get_master_interface;
static isc_sqlcode_t   p_isc_sqlcode;

#define BATCH_FLUSH_SIZE 1000

typedef struct
{
  struct IAttachment *att;
  struct ITransaction *tra;
  struct IStatus *st;
  struct IBatch *batch;
  struct IStatement *batch_stmt;
  struct IMessageMetadata *batch_meta;
  unsigned char *batch_buf;
  unsigned batch_buf_len;
  unsigned batch_count;
  char *batch_base_sql;
  int batch_auto_txn;
  char in_explicit_txn;  /* 1 if user sent BEGIN; clear on COMMIT/ROLLBACK */
} fb_conn_t;

typedef struct
{
  struct IStatement *stmt;
  struct IMessageMetadata *in_meta;
  struct IMessageMetadata *out_meta;
  unsigned in_buf_len;
  unsigned out_buf_len;
  unsigned char *in_buf;
  unsigned char *out_buf;
  unsigned nparams;
  unsigned nfields;
  char prepared;
  char cursor_open;
  struct IResultSet *cursor;
  db_value_t *cached_values;
  char **conv_bufs;
} fb_stmt_t;

typedef struct
{
  unsigned nrows;
  unsigned nfields;
  db_value_t *values;
  char **strings;
  unsigned nstrings;
  unsigned strings_capacity;
} fb_result_t;

static drv_caps_t firebird_drv_caps =
{
  0,    /* multi_rows_insert */
  1,    /* prepared_statements */
  0,    /* auto_increment */
  1,    /* needs_commit */
  0,    /* serial */
  0,    /* unsigned int */
};

static fb_drv_args_t args;
static char use_ps;
/* Set when the runtime libfbclient is older than FB4 — IStatement::createBatch
 * is absent from its vtable, so dispatching through that slot would crash. */
static char fb_no_batch;

static int firebird_drv_init(void);
static int firebird_drv_describe(drv_caps_t *);
static int firebird_drv_connect(db_conn_t *);
static int firebird_drv_disconnect(db_conn_t *);
static int firebird_drv_reconnect(db_conn_t *);
static int firebird_drv_prepare(db_stmt_t *, const char *, size_t);
static int firebird_drv_bind_param(db_stmt_t *, db_bind_t *, size_t);
static int firebird_drv_bind_result(db_stmt_t *, db_bind_t *, size_t);
static db_error_t firebird_drv_execute(db_stmt_t *, db_result_t *);
static int firebird_drv_fetch(db_result_t *);
static int firebird_drv_fetch_row(db_result_t *, db_row_t *);
static db_error_t firebird_drv_query(db_conn_t *, const char *, size_t,
                                     db_result_t *);
static int firebird_drv_free_results(db_result_t *);
static int firebird_drv_close(db_stmt_t *);
static int firebird_drv_done(void);

static db_driver_t firebird_driver =
{
  .sname = "firebird",
  .lname = "Firebird driver",
  .args = firebird_drv_args,
  .ops =
  {
    .init = firebird_drv_init,
    .describe = firebird_drv_describe,
    .connect = firebird_drv_connect,
    .disconnect = firebird_drv_disconnect,
    .reconnect = firebird_drv_reconnect,
    .prepare = firebird_drv_prepare,
    .bind_param = firebird_drv_bind_param,
    .bind_result = firebird_drv_bind_result,
    .execute = firebird_drv_execute,
    .fetch = firebird_drv_fetch,
    .fetch_row = firebird_drv_fetch_row,
    .free_results = firebird_drv_free_results,
    .close = firebird_drv_close,
    .query = firebird_drv_query,
    .done = firebird_drv_done
  }
};


/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int fb_check_status(struct IStatus *st)
{
  return (IStatus_getState(st) & IStatus_STATE_ERRORS) != 0;
}

static void fb_log_error(const char *func, struct IStatus *st)
{
  char msg[512];
  IUtil_formatStatus(fb_utl, msg, sizeof(msg), st);
  log_text(LOG_FATAL, "%s() failed: %s", func, msg);
}

static db_error_t fb_handle_error(db_conn_t *con, fb_conn_t *fbc,
                                  const char *func, const char *query,
                                  sb_counter_type_t *counter)
{
  char msg[512];
  IUtil_formatStatus(fb_utl, msg, sizeof(msg), fbc->st);

  long sqlcode = p_isc_sqlcode(IStatus_getErrors(fbc->st));

  char sqlstate_buf[16];
  snprintf(sqlstate_buf, sizeof(sqlstate_buf), "%05ld",
           sqlcode < 0 ? -sqlcode : sqlcode);

  con->sql_errno = (int)sqlcode;
  xfree(con->sql_state);
  xfree(con->sql_errmsg);
  con->sql_state = strdup(sqlstate_buf);
  con->sql_errmsg = strdup(msg);

  if (sqlcode == -913 || sqlcode == -803)
  {
    if (fbc->tra != NULL)
    {
      struct IStatus *rst = IMaster_getStatus(fb_master);
      ITransaction_rollback(fbc->tra, rst);
      IStatus_dispose(rst);
      fbc->tra = NULL;
    }
    *counter = SB_CNT_ERROR;
    return DB_ERROR_IGNORABLE;
  }

  log_text(LOG_FATAL, "%s() failed: SQLCODE %ld %s", func, sqlcode, msg);
  if (query != NULL)
    log_text(LOG_FATAL, "failed query was: %s", query);

  *counter = SB_CNT_ERROR;
  return DB_ERROR_FATAL;
}

/* Commit the implicit transaction after a successful statement when the
 * caller has not opened an explicit one with BEGIN. Matches MySQL/PgSQL
 * autocommit semantics so per-row cost (including fsync) is comparable. */
static int fb_autocommit_if_implicit(fb_conn_t *fbc)
{
  if (fbc->in_explicit_txn || fbc->tra == NULL)
    return 0;

  IStatus_init(fbc->st);
  ITransaction_commit(fbc->tra, fbc->st);
  fbc->tra = NULL;
  if (fb_check_status(fbc->st))
  {
    fb_log_error("autocommit", fbc->st);
    return 1;
  }
  return 0;
}

static int fb_ensure_transaction(fb_conn_t *fbc)
{
  if (fbc->tra != NULL)
    return 0;

  IStatus_init(fbc->st);
  fbc->tra = IAttachment_startTransaction(fbc->att, fbc->st, 0, NULL);
  if (fb_check_status(fbc->st))
  {
    fb_log_error("startTransaction", fbc->st);
    fbc->tra = NULL;
    return 1;
  }

  return 0;
}

static void fb_extract_column_fast(struct IMessageMetadata *meta,
                                   struct IStatus *st,
                                   unsigned char *buf,
                                   unsigned idx,
                                   db_value_t *val,
                                   char *conv_buf)
{
  unsigned nullOff = IMessageMetadata_getNullOffset(meta, st, idx);
  if (*(short *)(buf + nullOff) != 0)
  {
    val->ptr = NULL;
    val->len = 0;
    return;
  }

  unsigned off = IMessageMetadata_getOffset(meta, st, idx);
  unsigned type = IMessageMetadata_getType(meta, st, idx) & ~1;
  unsigned len = IMessageMetadata_getLength(meta, st, idx);

  switch (type)
  {
  case SQL_TEXT:
  {
    char *data = (char *)(buf + off);
    while (len > 0 && data[len - 1] == ' ')
      len--;
    val->ptr = data;
    val->len = len;
    return;
  }
  case SQL_VARYING:
  {
    ISC_USHORT vary_len;
    memcpy(&vary_len, buf + off, sizeof(ISC_USHORT));
    val->ptr = (const char *)(buf + off + sizeof(ISC_USHORT));
    val->len = vary_len;
    return;
  }
  case SQL_SHORT:
  {
    short v;
    memcpy(&v, buf + off, sizeof(short));
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "%d", (int)v);
    val->ptr = conv_buf;
    return;
  }
  case SQL_LONG:
  {
    ISC_LONG v;
    memcpy(&v, buf + off, sizeof(ISC_LONG));
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "%d", (int)v);
    val->ptr = conv_buf;
    return;
  }
  case SQL_INT64:
  {
    ISC_INT64 v;
    memcpy(&v, buf + off, sizeof(ISC_INT64));
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "%" PRId64,
                                  (int64_t)v);
    val->ptr = conv_buf;
    return;
  }
  case SQL_FLOAT:
  {
    float v;
    memcpy(&v, buf + off, sizeof(float));
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "%g", (double)v);
    val->ptr = conv_buf;
    return;
  }
  case SQL_DOUBLE:
  {
    double v;
    memcpy(&v, buf + off, sizeof(double));
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "%g", v);
    val->ptr = conv_buf;
    return;
  }
  default:
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "?");
    val->ptr = conv_buf;
    return;
  }
}

static void fb_result_add_string(fb_result_t *fbrs, char *s)
{
  if (s == NULL)
    return;
  if (fbrs->nstrings >= fbrs->strings_capacity)
  {
    unsigned new_cap = fbrs->strings_capacity ? fbrs->strings_capacity * 2 : 64;
    char **new_arr = realloc(fbrs->strings, new_cap * sizeof(char *));
    if (new_arr == NULL) { free(s); return; }
    fbrs->strings = new_arr;
    fbrs->strings_capacity = new_cap;
  }
  fbrs->strings[fbrs->nstrings++] = s;
}

static char *fb_extract_column_alloc(struct IMessageMetadata *meta,
                                     struct IStatus *st,
                                     unsigned char *buf,
                                     unsigned idx,
                                     uint32_t *out_len)
{
  unsigned nullOff = IMessageMetadata_getNullOffset(meta, st, idx);
  if (*(short *)(buf + nullOff) != 0)
  {
    *out_len = 0;
    return NULL;
  }

  unsigned off = IMessageMetadata_getOffset(meta, st, idx);
  unsigned type = IMessageMetadata_getType(meta, st, idx) & ~1;
  unsigned len = IMessageMetadata_getLength(meta, st, idx);
  char tmp[MAX_COLUMN_LENGTH];
  int n;

  switch (type)
  {
  case SQL_TEXT:
  {
    char *data = (char *)(buf + off);
    while (len > 0 && data[len - 1] == ' ')
      len--;
    *out_len = len;
    char *s = malloc(len + 1);
    if (s) { memcpy(s, data, len); s[len] = '\0'; }
    return s;
  }
  case SQL_VARYING:
  {
    ISC_USHORT vary_len;
    memcpy(&vary_len, buf + off, sizeof(ISC_USHORT));
    *out_len = vary_len;
    char *s = malloc(vary_len + 1);
    if (s) { memcpy(s, buf + off + sizeof(ISC_USHORT), vary_len); s[vary_len] = '\0'; }
    return s;
  }
  case SQL_SHORT:
  {
    short v; memcpy(&v, buf + off, sizeof(short));
    n = snprintf(tmp, sizeof(tmp), "%d", (int)v);
    break;
  }
  case SQL_LONG:
  {
    ISC_LONG v; memcpy(&v, buf + off, sizeof(ISC_LONG));
    n = snprintf(tmp, sizeof(tmp), "%d", (int)v);
    break;
  }
  case SQL_INT64:
  {
    ISC_INT64 v; memcpy(&v, buf + off, sizeof(ISC_INT64));
    n = snprintf(tmp, sizeof(tmp), "%" PRId64, (int64_t)v);
    break;
  }
  case SQL_FLOAT:
  {
    float v; memcpy(&v, buf + off, sizeof(float));
    n = snprintf(tmp, sizeof(tmp), "%g", (double)v);
    break;
  }
  case SQL_DOUBLE:
  {
    double v; memcpy(&v, buf + off, sizeof(double));
    n = snprintf(tmp, sizeof(tmp), "%g", v);
    break;
  }
  default:
    n = snprintf(tmp, sizeof(tmp), "?");
    break;
  }

  if (n < 0) n = 0;
  *out_len = (uint32_t)n;
  char *s = malloc(n + 1);
  if (s) { memcpy(s, tmp, n); s[n] = '\0'; }
  return s;
}

static void fb_free_result(fb_result_t *fbrs)
{
  if (fbrs == NULL)
    return;
  for (unsigned i = 0; i < fbrs->nstrings; i++)
    free(fbrs->strings[i]);
  free(fbrs->strings);
  free(fbrs->values);
  free(fbrs);
}


/* ------------------------------------------------------------------ */
/* Batch insert helpers                                               */
/* ------------------------------------------------------------------ */

static int fb_batch_flush(fb_conn_t *fbc)
{
  if (fbc->batch == NULL || fbc->batch_count == 0)
    return 0;

  IStatus_init(fbc->st);
  struct IBatchCompletionState *cs = IBatch_execute(fbc->batch, fbc->st,
                                                     fbc->tra);
  if (cs)
    IBatchCompletionState_dispose(cs);

  if (fb_check_status(fbc->st))
  {
    fb_log_error("IBatch_execute", fbc->st);
    return 1;
  }

  fbc->batch_count = 0;

  if (fbc->batch_auto_txn && fbc->tra != NULL)
  {
    IStatus_init(fbc->st);
    ITransaction_commitRetaining(fbc->tra, fbc->st);
  }

  return 0;
}

static void fb_batch_close(fb_conn_t *fbc)
{
  if (fbc->batch != NULL)
  {
    if (fbc->batch_count > 0)
      fb_batch_flush(fbc);
    IStatus_init(fbc->st);
    IBatch_close(fbc->batch, fbc->st);
    fbc->batch = NULL;
  }
  if (fbc->batch_stmt != NULL)
  {
    IStatement_free(fbc->batch_stmt, fbc->st);
    fbc->batch_stmt = NULL;
  }
  if (fbc->batch_meta != NULL)
  {
    IMessageMetadata_release(fbc->batch_meta);
    fbc->batch_meta = NULL;
  }
  free(fbc->batch_buf);
  fbc->batch_buf = NULL;
  fbc->batch_buf_len = 0;
  fbc->batch_count = 0;
  free(fbc->batch_base_sql);
  fbc->batch_base_sql = NULL;
}

static int fb_batch_create(fb_conn_t *fbc, const char *base_sql,
                            unsigned ncols)
{
  char param_sql[1024];
  char params[256];
  char *p = params;

  if (fb_no_batch)
    return -1;  /* runtime libfbclient is too old for IStatement::createBatch */

  if (ncols > 120)
    return 1;

  *p++ = '(';
  for (unsigned i = 0; i < ncols; i++)
  {
    if (i > 0) *p++ = ',';
    *p++ = '?';
  }
  *p++ = ')';
  *p = '\0';

  snprintf(param_sql, sizeof(param_sql), "%s%s", base_sql, params);

  if (fb_ensure_transaction(fbc))
    return 1;

  IStatus_init(fbc->st);
  struct IStatement *batch_stmt = IAttachment_prepare(fbc->att, fbc->st,
      fbc->tra, 0, param_sql, FB_DIALECT, IStatement_PREPARE_PREFETCH_METADATA);

  if (fb_check_status(fbc->st))
  {
    fb_log_error("prepare(batch)", fbc->st);
    return 1;
  }

  struct IMessageMetadata *in_meta = IStatement_getInputMetadata(batch_stmt,
                                                                  fbc->st);

  IStatus_init(fbc->st);
  fbc->batch = IStatement_createBatch(batch_stmt, fbc->st, in_meta, 0, NULL);

  if (fb_check_status(fbc->st))
  {
    IMessageMetadata_release(in_meta);
    IStatement_free(batch_stmt, fbc->st);
    fbc->batch = NULL;
    return -1;
  }

  fbc->batch_stmt = batch_stmt;

  fbc->batch_meta = in_meta;
  fbc->batch_buf_len = IMessageMetadata_getMessageLength(fbc->batch_meta,
                                                          fbc->st);
  fbc->batch_buf = (unsigned char *)calloc(1, fbc->batch_buf_len);
  if (fbc->batch_buf == NULL)
  {
    fb_batch_close(fbc);
    return 1;
  }

  fbc->batch_base_sql = strdup(base_sql);
  fbc->batch_count = 0;

  return 0;
}

static int fb_batch_add_row(fb_conn_t *fbc, const char *values_str)
{
  memset(fbc->batch_buf, 0, fbc->batch_buf_len);

  unsigned ncols = IMessageMetadata_getCount(fbc->batch_meta, fbc->st);
  const char *p = values_str;

  while (*p && *p != '(') p++;
  if (*p == '(') p++;

  for (unsigned col = 0; col < ncols; col++)
  {
    while (*p == ' ') p++;

    unsigned off = IMessageMetadata_getOffset(fbc->batch_meta, fbc->st, col);
    unsigned nullOff = IMessageMetadata_getNullOffset(fbc->batch_meta, fbc->st, col);
    unsigned type = IMessageMetadata_getType(fbc->batch_meta, fbc->st, col) & ~1;
    unsigned meta_len = IMessageMetadata_getLength(fbc->batch_meta, fbc->st, col);

    *(short *)(fbc->batch_buf + nullOff) = 0;

    if (*p == '\'')
    {
      p++;
      char tmp[4096];
      unsigned slen = 0;
      while (*p)
      {
        if (*p == '\'')
        {
          if (*(p + 1) == '\'')
          {
            if (slen < sizeof(tmp)) tmp[slen++] = '\'';
            p += 2;
            continue;
          }
          break;
        }
        if (slen < sizeof(tmp)) tmp[slen++] = *p;
        p++;
      }
      if (*p == '\'') p++;

      if (type == SQL_TEXT)
      {
        unsigned copy = slen < meta_len ? slen : meta_len;
        memcpy(fbc->batch_buf + off, tmp, copy);
        if (copy < meta_len)
          memset(fbc->batch_buf + off + copy, ' ', meta_len - copy);
      }
      else
      {
        if (slen > meta_len) slen = meta_len;
        ISC_USHORT vary_len = (ISC_USHORT)slen;
        memcpy(fbc->batch_buf + off, &vary_len, sizeof(ISC_USHORT));
        memcpy(fbc->batch_buf + off + sizeof(ISC_USHORT), tmp, slen);
      }
    }
    else if (strncasecmp(p, "NULL", 4) == 0 &&
             (p[4] == ',' || p[4] == ')' || p[4] == ' ' || p[4] == '\0'))
    {
      *(short *)(fbc->batch_buf + nullOff) = -1;
      while (*p && *p != ',' && *p != ')') p++;
    }
    else
    {
      const char *start = p;
      while (*p && *p != ',' && *p != ')') p++;
      unsigned slen = (unsigned)(p - start);
      while (slen > 0 && start[slen - 1] == ' ') slen--;

      if (type == SQL_LONG)
      {
        ISC_LONG val = (ISC_LONG)atoi(start);
        memcpy(fbc->batch_buf + off, &val, sizeof(ISC_LONG));
      }
      else if (type == SQL_INT64)
      {
        ISC_INT64 val = (ISC_INT64)atoll(start);
        memcpy(fbc->batch_buf + off, &val, sizeof(ISC_INT64));
      }
      else if (type == SQL_SHORT)
      {
        short val = (short)atoi(start);
        memcpy(fbc->batch_buf + off, &val, sizeof(short));
      }
      else if (type == SQL_FLOAT)
      {
        float val = (float)atof(start);
        memcpy(fbc->batch_buf + off, &val, sizeof(float));
      }
      else if (type == SQL_DOUBLE)
      {
        double val = atof(start);
        memcpy(fbc->batch_buf + off, &val, sizeof(double));
      }
      else
      {
        ISC_LONG val = (ISC_LONG)atoi(start);
        memcpy(fbc->batch_buf + off, &val, sizeof(ISC_LONG));
      }
    }

    while (*p == ' ' || *p == ',') p++;
  }

  IStatus_init(fbc->st);
  IBatch_add(fbc->batch, fbc->st, 1, fbc->batch_buf);

  if (fb_check_status(fbc->st))
  {
    fb_log_error("IBatch_add", fbc->st);
    return 1;
  }

  fbc->batch_count++;

  if (fbc->batch_count >= BATCH_FLUSH_SIZE)
    return fb_batch_flush(fbc);

  return 0;
}


/* ------------------------------------------------------------------ */
/* Driver interface                                                   */
/* ------------------------------------------------------------------ */

int register_driver_firebird(sb_list_t *drivers)
{
  SB_LIST_ADD_TAIL(&firebird_driver.listitem, drivers);
  return 0;
}


int firebird_drv_init(void)
{
  args.db = sb_get_value_string("firebird-db");
  args.user = sb_get_value_string("firebird-user");
  args.password = sb_get_value_string("firebird-password");
  args.client = sb_get_value_string("firebird-client");

  fb_dlhandle = dlopen(args.client, RTLD_NOW | RTLD_GLOBAL);
  if (fb_dlhandle == NULL)
  {
    log_text(LOG_FATAL, "dlopen('%s') failed: %s", args.client, dlerror());
    log_text(LOG_FATAL,
             "set --firebird-client=PATH or LD_LIBRARY_PATH to the directory "
             "containing libfbclient.so");
    return 1;
  }

  p_fb_get_master_interface =
      (fb_get_master_t)dlsym(fb_dlhandle, "fb_get_master_interface");
  p_isc_sqlcode = (isc_sqlcode_t)dlsym(fb_dlhandle, "isc_sqlcode");
  if (p_fb_get_master_interface == NULL || p_isc_sqlcode == NULL)
  {
    log_text(LOG_FATAL,
             "dlsym failed in '%s': %s — is this really libfbclient?",
             args.client, dlerror());
    return 1;
  }

  fb_master = p_fb_get_master_interface();
  fb_prov = IMaster_getDispatcher(fb_master);
  fb_utl = IMaster_getUtilInterface(fb_master);

  /* Runtime IUtil vtable version. The cloop ABI guarantees vtable->version
   * sits at a fixed offset that's safe to read without dispatch. The C API
   * wrapper (fb_c_api.h) was introduced in Firebird 5; FB3 and earlier
   * ship only C++ interface headers and use an older vtable layout. Many
   * methods we dispatch through (createBatch, immediate-execute forms,
   * etc.) sit at offsets that don't exist in those older vtables and would
   * crash. */
  {
    uintptr_t util_vt_version =
        ((uintptr_t *)((void **)fb_utl->vtable))[1];
    if (util_vt_version < 3)
    {
      log_text(LOG_FATAL,
               "Firebird client library is too old for this driver "
               "(IUtil vtable v%u). The OO API driver requires libfbclient "
               "from Firebird 4.0 or newer.",
               (unsigned)util_vt_version);
      log_text(LOG_FATAL,
               "For Firebird 3 support, use the `firebird-isc` branch which "
               "uses the legacy ISC API.");
      return 1;
    }
    if (util_vt_version < 4)
    {
      fb_no_batch = 1;
      log_text(LOG_NOTICE,
               "Firebird client lib pre-4.0 (IUtil vtable v%u) — "
               "Batch API disabled; bulk inserts fall back to single-row.",
               (unsigned)util_vt_version);
    }
  }

  /* Detect embedded mode: a connection string with no "<host>[/<port>]:"
   * prefix makes libfbclient load the embedded engine in-process. The
   * embedded engine builds a deep object hierarchy on the worker-thread
   * stack during attachDatabase and segfaults with sysbench's default 64K
   * thread stack. Networked syntax is `host:path` or `host/port:path`. */
  {
    int is_embedded = (args.db[0] == '/' || args.db[0] == '.'
                       || strchr(args.db, ':') == NULL);
    if (is_embedded)
    {
      size_t stack_size = sb_get_value_size("thread-stack-size");
      if (stack_size < 2 * 1024 * 1024)
      {
        log_text(LOG_WARNING,
                 "Firebird embedded mode detected ('%s' has no host: prefix).",
                 args.db);
        log_text(LOG_WARNING,
                 "The embedded engine needs more stack than sysbench's default "
                 "64K — run with --thread-stack-size=2M or larger, otherwise "
                 "worker threads will segfault inside libEngine*.so during "
                 "attachDatabase.");
      }
    }
  }

  use_ps = 0;
  firebird_drv_caps.prepared_statements = 1;
  if (db_globals.ps_mode != DB_PS_MODE_DISABLE)
    use_ps = 1;

  return 0;
}


int firebird_drv_describe(drv_caps_t *caps)
{
  *caps = firebird_drv_caps;
  return 0;
}


int firebird_drv_connect(db_conn_t *sb_conn)
{
  fb_conn_t *fbc = (fb_conn_t *)calloc(1, sizeof(fb_conn_t));
  if (fbc == NULL)
    return 1;

  fbc->st = IMaster_getStatus(fb_master);
  IStatus_init(fbc->st);

  struct IXpbBuilder *dpb = IUtil_getXpbBuilder(fb_utl, fbc->st,
                                                 IXpbBuilder_DPB, NULL, 0);
  if (fb_check_status(fbc->st))
    goto error;

  IXpbBuilder_insertString(dpb, fbc->st, isc_dpb_user_name, args.user);
  IXpbBuilder_insertString(dpb, fbc->st, isc_dpb_password, args.password);
  IXpbBuilder_insertString(dpb, fbc->st, isc_dpb_lc_ctype, "UTF8");

  if (fb_check_status(fbc->st))
  {
    IXpbBuilder_dispose(dpb);
    goto error;
  }

  fbc->att = IProvider_attachDatabase(fb_prov, fbc->st, args.db,
      IXpbBuilder_getBufferLength(dpb, fbc->st),
      IXpbBuilder_getBuffer(dpb, fbc->st));

  IXpbBuilder_dispose(dpb);

  if (fb_check_status(fbc->st))
  {
    fb_log_error("attachDatabase", fbc->st);
    goto error;
  }

  fbc->tra = NULL;
  sb_conn->ptr = fbc;
  return 0;

error:
  if (fbc->st) IStatus_dispose(fbc->st);
  free(fbc);
  return 1;
}


int firebird_drv_disconnect(db_conn_t *sb_conn)
{
  fb_conn_t *fbc = (fb_conn_t *)sb_conn->ptr;

  xfree(sb_conn->sql_state);
  xfree(sb_conn->sql_errmsg);

  if (fbc == NULL)
    return 0;

  fb_batch_close(fbc);

  if (fbc->tra != NULL)
  {
    ITransaction_rollback(fbc->tra, fbc->st);
    fbc->tra = NULL;
  }

  if (fbc->att != NULL)
  {
    IAttachment_detach(fbc->att, fbc->st);
    fbc->att = NULL;
  }

  IStatus_dispose(fbc->st);
  free(fbc);
  sb_conn->ptr = NULL;

  return 0;
}


int firebird_drv_reconnect(db_conn_t *sb_conn)
{
  if (firebird_drv_disconnect(sb_conn))
    return DB_ERROR_FATAL;

  while (firebird_drv_connect(sb_conn))
  {
    if (sb_globals.error)
      return DB_ERROR_FATAL;
  }

  return DB_ERROR_IGNORABLE;
}


int firebird_drv_prepare(db_stmt_t *stmt, const char *query, size_t len)
{
  fb_conn_t *fbc = (fb_conn_t *)stmt->connection->ptr;
  fb_stmt_t *fbstmt = NULL;

  (void)len;

  if (fbc == NULL)
    return 1;

  if (!use_ps ||
      strcasecmp(query, "BEGIN") == 0 ||
      strcasecmp(query, "COMMIT") == 0 ||
      strcasecmp(query, "ROLLBACK") == 0)
  {
    stmt->emulated = 1;
    stmt->query = strdup(query);
    return 0;
  }

  fbstmt = (fb_stmt_t *)calloc(1, sizeof(fb_stmt_t));
  if (fbstmt == NULL)
    return 1;

  if (fb_ensure_transaction(fbc))
    goto error;

  IStatus_init(fbc->st);
  fbstmt->stmt = IAttachment_prepare(fbc->att, fbc->st, fbc->tra, 0, query,
                                      FB_DIALECT,
                                      IStatement_PREPARE_PREFETCH_METADATA);
  if (fb_check_status(fbc->st))
  {
    fb_log_error("prepare", fbc->st);
    goto error;
  }

  fbstmt->out_meta = IStatement_getOutputMetadata(fbstmt->stmt, fbc->st);
  fbstmt->in_meta = IStatement_getInputMetadata(fbstmt->stmt, fbc->st);

  fbstmt->nfields = IMessageMetadata_getCount(fbstmt->out_meta, fbc->st);
  fbstmt->nparams = IMessageMetadata_getCount(fbstmt->in_meta, fbc->st);

  fbstmt->out_buf_len = IMessageMetadata_getMessageLength(fbstmt->out_meta,
                                                           fbc->st);
  fbstmt->in_buf_len = IMessageMetadata_getMessageLength(fbstmt->in_meta,
                                                          fbc->st);

  if (fbstmt->out_buf_len > 0)
  {
    fbstmt->out_buf = (unsigned char *)calloc(1, fbstmt->out_buf_len);
    if (fbstmt->out_buf == NULL)
      goto error;
  }

  if (fbstmt->in_buf_len > 0)
  {
    fbstmt->in_buf = (unsigned char *)calloc(1, fbstmt->in_buf_len);
    if (fbstmt->in_buf == NULL)
      goto error;
  }

  if (fbstmt->nfields > 0)
  {
    fbstmt->cached_values = (db_value_t *)calloc(fbstmt->nfields,
                                                  sizeof(db_value_t));
    fbstmt->conv_bufs = (char **)calloc(fbstmt->nfields, sizeof(char *));
    if (fbstmt->cached_values == NULL || fbstmt->conv_bufs == NULL)
      goto error;
    for (unsigned i = 0; i < fbstmt->nfields; i++)
    {
      fbstmt->conv_bufs[i] = (char *)malloc(MAX_COLUMN_LENGTH);
      if (fbstmt->conv_bufs[i] == NULL)
        goto error;
    }
  }

  fbstmt->prepared = 1;
  stmt->ptr = fbstmt;
  stmt->query = strdup(query);

  return 0;

error:
  if (fbstmt != NULL)
  {
    if (fbstmt->conv_bufs != NULL)
    {
      for (unsigned i = 0; i < fbstmt->nfields; i++)
        free(fbstmt->conv_bufs[i]);
      free(fbstmt->conv_bufs);
    }
    free(fbstmt->cached_values);
    free(fbstmt->out_buf);
    free(fbstmt->in_buf);
    if (fbstmt->out_meta) IMessageMetadata_release(fbstmt->out_meta);
    if (fbstmt->in_meta) IMessageMetadata_release(fbstmt->in_meta);
    if (fbstmt->stmt) IStatement_free(fbstmt->stmt, fbc->st);
    free(fbstmt);
  }
  return 1;
}


int firebird_drv_bind_param(db_stmt_t *stmt, db_bind_t *params, size_t len)
{
  fb_stmt_t *fbstmt;

  if (stmt->bound_param != NULL)
    free(stmt->bound_param);
  stmt->bound_param = (db_bind_t *)malloc(len * sizeof(db_bind_t));
  if (stmt->bound_param == NULL)
    return 1;
  memcpy(stmt->bound_param, params, len * sizeof(db_bind_t));
  stmt->bound_param_len = len;

  if (stmt->emulated)
    return 0;

  fbstmt = (fb_stmt_t *)stmt->ptr;
  if (fbstmt == NULL || !fbstmt->prepared)
    return 1;

  if (fbstmt->nparams != (unsigned)len)
  {
    log_text(LOG_ALERT, "wrong number of parameters in prepared statement");
    return 1;
  }

  return 0;
}


int firebird_drv_bind_result(db_stmt_t *stmt, db_bind_t *params, size_t len)
{
  (void)stmt; (void)params; (void)len;
  return 0;
}


static void fb_set_param(fb_stmt_t *fbstmt, struct IStatus *st,
                         unsigned idx, db_bind_t *bind)
{
  unsigned off = IMessageMetadata_getOffset(fbstmt->in_meta, st, idx);
  unsigned nullOff = IMessageMetadata_getNullOffset(fbstmt->in_meta, st, idx);

  if (bind->is_null && *bind->is_null)
  {
    *(short *)(fbstmt->in_buf + nullOff) = -1;
    return;
  }

  *(short *)(fbstmt->in_buf + nullOff) = 0;

  switch (bind->type)
  {
  case DB_TYPE_TINYINT:
  case DB_TYPE_SMALLINT:
  {
    short val = (bind->type == DB_TYPE_TINYINT)
      ? (short)(*(char *)bind->buffer)
      : *(short *)bind->buffer;
    memcpy(fbstmt->in_buf + off, &val, sizeof(short));
    break;
  }
  case DB_TYPE_INT:
  {
    ISC_LONG val = *(int *)bind->buffer;
    memcpy(fbstmt->in_buf + off, &val, sizeof(ISC_LONG));
    break;
  }
  case DB_TYPE_BIGINT:
  {
    ISC_INT64 val = *(long long *)bind->buffer;
    memcpy(fbstmt->in_buf + off, &val, sizeof(ISC_INT64));
    break;
  }
  case DB_TYPE_FLOAT:
  {
    float val = *(float *)bind->buffer;
    memcpy(fbstmt->in_buf + off, &val, sizeof(float));
    break;
  }
  case DB_TYPE_DOUBLE:
  {
    double val = *(double *)bind->buffer;
    memcpy(fbstmt->in_buf + off, &val, sizeof(double));
    break;
  }
  case DB_TYPE_CHAR:
  case DB_TYPE_VARCHAR:
  {
    unsigned long data_len = bind->data_len
      ? *bind->data_len
      : strlen((char *)bind->buffer);
    unsigned meta_type = IMessageMetadata_getType(fbstmt->in_meta, st, idx) & ~1;
    unsigned meta_len = IMessageMetadata_getLength(fbstmt->in_meta, st, idx);

    if (meta_type == SQL_TEXT)
    {
      unsigned copy_len = data_len < meta_len ? data_len : meta_len;
      memcpy(fbstmt->in_buf + off, bind->buffer, copy_len);
      if (copy_len < meta_len)
        memset(fbstmt->in_buf + off + copy_len, ' ', meta_len - copy_len);
    }
    else
    {
      if (data_len > meta_len) data_len = meta_len;
      ISC_USHORT vary_len = (ISC_USHORT)data_len;
      memcpy(fbstmt->in_buf + off, &vary_len, sizeof(ISC_USHORT));
      memcpy(fbstmt->in_buf + off + sizeof(ISC_USHORT), bind->buffer, data_len);
    }
    break;
  }
  default:
  {
    char buf[256];
    int n = db_print_value(bind, buf, sizeof(buf));
    if (n > 0)
    {
      unsigned meta_type = IMessageMetadata_getType(fbstmt->in_meta, st, idx) & ~1;
      unsigned meta_len = IMessageMetadata_getLength(fbstmt->in_meta, st, idx);
      if (meta_type == SQL_TEXT)
      {
        unsigned copy_len = (unsigned)n < meta_len ? (unsigned)n : meta_len;
        memcpy(fbstmt->in_buf + off, buf, copy_len);
        if (copy_len < meta_len)
          memset(fbstmt->in_buf + off + copy_len, ' ', meta_len - copy_len);
      }
      else
      {
        if ((unsigned)n > meta_len) n = (int)meta_len;
        ISC_USHORT vary_len = (ISC_USHORT)n;
        memcpy(fbstmt->in_buf + off, &vary_len, sizeof(ISC_USHORT));
        memcpy(fbstmt->in_buf + off + sizeof(ISC_USHORT), buf, n);
      }
    }
    break;
  }
  }
}


db_error_t firebird_drv_execute(db_stmt_t *stmt, db_result_t *rs)
{
  db_conn_t *con = stmt->connection;
  fb_conn_t *fbc = (fb_conn_t *)con->ptr;
  fb_stmt_t *fbstmt;
  char *buf = NULL;
  unsigned int buflen = 0;
  unsigned int i, j, vcnt;
  char need_realloc;
  int n;

  con->sql_errno = 0;
  xfree(con->sql_state);
  xfree(con->sql_errmsg);

  if (!stmt->emulated)
  {
    fbstmt = (fb_stmt_t *)stmt->ptr;
    if (fbstmt == NULL || !fbstmt->prepared)
    {
      log_text(LOG_DEBUG, "ERROR: uninitialized Firebird statement");
      return DB_ERROR_FATAL;
    }

    if (fbstmt->cursor_open)
    {
      IResultSet_close(fbstmt->cursor, fbc->st);
      fbstmt->cursor = NULL;
      fbstmt->cursor_open = 0;
    }

    for (i = 0; i < fbstmt->nparams; i++)
      fb_set_param(fbstmt, fbc->st, i, &stmt->bound_param[i]);

    if (fb_ensure_transaction(fbc))
      return DB_ERROR_FATAL;

    IStatus_init(fbc->st);

    if (fbstmt->nfields > 0)
    {
      fbstmt->cursor = IStatement_openCursor(fbstmt->stmt, fbc->st, fbc->tra,
          fbstmt->nparams > 0 ? fbstmt->in_meta : NULL,
          fbstmt->nparams > 0 ? fbstmt->in_buf : NULL,
          fbstmt->out_meta, 0);

      if (fb_check_status(fbc->st))
        return fb_handle_error(con, fbc, "openCursor", stmt->query,
                               &rs->counter);

      fbstmt->cursor_open = 1;
      rs->counter = SB_CNT_READ;

      int fetch_rc = IResultSet_fetchNext(fbstmt->cursor, fbc->st,
                                           fbstmt->out_buf);

      if (fetch_rc == IStatus_RESULT_OK)
      {
        for (unsigned ci = 0; ci < fbstmt->nfields; ci++)
          fb_extract_column_fast(fbstmt->out_meta, fbc->st, fbstmt->out_buf,
                                  ci, &fbstmt->cached_values[ci],
                                  fbstmt->conv_bufs[ci]);

        unsigned char *row0_buf = malloc(fbstmt->out_buf_len);
        if (row0_buf != NULL)
          memcpy(row0_buf, fbstmt->out_buf, fbstmt->out_buf_len);

        int second = IResultSet_fetchNext(fbstmt->cursor, fbc->st,
                                           fbstmt->out_buf);

        if (second == IStatus_RESULT_NO_DATA)
        {
          free(row0_buf);
          rs->nrows = 1;
          rs->nfields = fbstmt->nfields;
          rs->ptr = fbstmt;
          return DB_ERROR_NONE;
        }

        if (second == IStatus_RESULT_OK)
        {
          fb_result_t *fbrs = (fb_result_t *)calloc(1, sizeof(fb_result_t));
          if (fbrs == NULL)
          {
            free(row0_buf);
            return DB_ERROR_FATAL;
          }
          unsigned capacity = 64;
          fbrs->nfields = fbstmt->nfields;
          fbrs->values = (db_value_t *)calloc(capacity * fbstmt->nfields,
                                               sizeof(db_value_t));
          if (fbrs->values == NULL)
          {
            free(row0_buf);
            free(fbrs);
            return DB_ERROR_FATAL;
          }

          /* Row 0: re-extract from row0_buf snapshot taken before second fetch */
          for (unsigned ci = 0; ci < fbstmt->nfields; ci++)
          {
            uint32_t col_len = 0;
            char *s = fb_extract_column_alloc(fbstmt->out_meta, fbc->st,
                row0_buf, ci, &col_len);
            fbrs->values[ci].ptr = s;
            fbrs->values[ci].len = col_len;
            fb_result_add_string(fbrs, s);
          }
          free(row0_buf);
          unsigned nrows = 1;

          /* Row 1: extract from current out_buf (second fetch result) */
          if (nrows >= capacity)
          {
            capacity *= 2;
            db_value_t *nv = realloc(fbrs->values,
                capacity * fbstmt->nfields * sizeof(db_value_t));
            if (nv == NULL) { fb_free_result(fbrs); return DB_ERROR_FATAL; }
            fbrs->values = nv;
          }
          for (unsigned ci = 0; ci < fbstmt->nfields; ci++)
          {
            uint32_t col_len = 0;
            char *s = fb_extract_column_alloc(fbstmt->out_meta, fbc->st,
                fbstmt->out_buf, ci, &col_len);
            fbrs->values[nrows * fbstmt->nfields + ci].ptr = s;
            fbrs->values[nrows * fbstmt->nfields + ci].len = col_len;
            fb_result_add_string(fbrs, s);
          }
          nrows++;

          while ((fetch_rc = IResultSet_fetchNext(fbstmt->cursor, fbc->st,
                                                   fbstmt->out_buf))
                 == IStatus_RESULT_OK)
          {
            if (nrows >= capacity)
            {
              capacity *= 2;
              db_value_t *nv = realloc(fbrs->values,
                  capacity * fbstmt->nfields * sizeof(db_value_t));
              if (nv == NULL) { fb_free_result(fbrs); return DB_ERROR_FATAL; }
              fbrs->values = nv;
            }
            for (unsigned ci = 0; ci < fbstmt->nfields; ci++)
            {
              uint32_t col_len = 0;
              char *s = fb_extract_column_alloc(fbstmt->out_meta, fbc->st,
                  fbstmt->out_buf, ci, &col_len);
              fbrs->values[nrows * fbstmt->nfields + ci].ptr = s;
              fbrs->values[nrows * fbstmt->nfields + ci].len = col_len;
              fb_result_add_string(fbrs, s);
            }
            nrows++;
          }

          fbrs->nrows = nrows;
          rs->nrows = nrows;
          rs->nfields = fbstmt->nfields;
          rs->ptr = fbrs;
          return DB_ERROR_NONE;
        }

        free(row0_buf);
        return fb_handle_error(con, fbc, "fetchNext", stmt->query,
                               &rs->counter);
      }

      if (fetch_rc == IStatus_RESULT_NO_DATA)
      {
        rs->nrows = 0;
        rs->nfields = fbstmt->nfields;
        rs->ptr = fbstmt;
        return DB_ERROR_NONE;
      }

      return fb_handle_error(con, fbc, "fetchNext", stmt->query,
                             &rs->counter);
    }

    IStatement_execute(fbstmt->stmt, fbc->st, fbc->tra,
        fbstmt->nparams > 0 ? fbstmt->in_meta : NULL,
        fbstmt->nparams > 0 ? fbstmt->in_buf : NULL,
        NULL, NULL);

    if (fb_check_status(fbc->st))
      return fb_handle_error(con, fbc, "execute", stmt->query, &rs->counter);

    if (fb_autocommit_if_implicit(fbc))
      return DB_ERROR_FATAL;

    rs->counter = SB_CNT_WRITE;
    rs->nrows = 1;
    return DB_ERROR_NONE;
  }

  /* Emulated prepared statements */
  need_realloc = 1;
  vcnt = 0;
  for (i = 0, j = 0; stmt->query[i] != '\0'; i++)
  {
  again:
    if (j + 1 >= buflen || need_realloc)
    {
      buflen = (buflen > 0) ? buflen * 2 : 256;
      buf = realloc(buf, buflen);
      if (buf == NULL)
        return DB_ERROR_FATAL;
      need_realloc = 0;
    }

    if (stmt->query[i] != '?')
    {
      buf[j++] = stmt->query[i];
      continue;
    }

    n = db_print_value(stmt->bound_param + vcnt, buf + j, buflen - j);
    if (n < 0)
    {
      need_realloc = 1;
      goto again;
    }
    j += n;
    vcnt++;
  }
  buf[j] = '\0';

  db_error_t rc = firebird_drv_query(con, buf, j, rs);
  free(buf);
  return rc;
}


db_error_t firebird_drv_query(db_conn_t *sb_conn, const char *query, size_t len,
                              db_result_t *rs)
{
  fb_conn_t *fbc = (fb_conn_t *)sb_conn->ptr;

  (void)len;

  sb_conn->sql_errno = 0;
  xfree(sb_conn->sql_state);
  xfree(sb_conn->sql_errmsg);

  /* Transaction control intercept.
   * Firebird accepts COMMIT and ROLLBACK as plain SQL, but we capture them
   * here to (a) flush any pending IBatch before commit and (b) reset our
   * fbc->tra pointer after the OO API ITransaction handle is consumed.
   * BEGIN is sysbench-specific; the Firebird equivalent is SET TRANSACTION,
   * but since we have ITransaction_startTransaction at hand it's simpler to
   * call it directly than to round-trip through SQL.
   */
  int is_commit = (strcasecmp(query, "COMMIT") == 0);
  int is_rollback = (strcasecmp(query, "ROLLBACK") == 0);

  if (is_commit || is_rollback || strcasecmp(query, "BEGIN") == 0)
  {
    if (is_commit)
      fb_batch_close(fbc);

    if (fbc->tra != NULL)
    {
      IStatus_init(fbc->st);
      if (is_rollback)
        ITransaction_rollback(fbc->tra, fbc->st);
      else
        ITransaction_commit(fbc->tra, fbc->st);
      fbc->tra = NULL;
      if (!is_rollback && fb_check_status(fbc->st))
      {
        fb_log_error(is_commit ? "commit" : "commit(implicit in BEGIN)",
                     fbc->st);
        rs->counter = SB_CNT_ERROR;
        return DB_ERROR_FATAL;
      }
    }

    if (!is_commit && !is_rollback)
    {
      IStatus_init(fbc->st);
      fbc->tra = IAttachment_startTransaction(fbc->att, fbc->st, 0, NULL);
      if (fb_check_status(fbc->st))
      {
        fb_log_error("startTransaction", fbc->st);
        fbc->tra = NULL;
        rs->counter = SB_CNT_ERROR;
        return DB_ERROR_FATAL;
      }
      fbc->in_explicit_txn = 1;
    }
    else
    {
      fbc->in_explicit_txn = 0;
    }

    rs->counter = SB_CNT_OTHER;
    rs->nrows = 0;
    return DB_ERROR_NONE;
  }

  /* Handle CREATE TABLE IF NOT EXISTS */
  {
    const char *q = query;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
      q++;
    if (strncasecmp(q, "CREATE TABLE IF NOT EXISTS ", 27) == 0)
    {
      char create_buf[4096];
      snprintf(create_buf, sizeof(create_buf), "CREATE TABLE %s", q + 27);

      int auto_txn = (fbc->tra == NULL);
      if (auto_txn && fb_ensure_transaction(fbc))
      {
        rs->counter = SB_CNT_ERROR;
        return DB_ERROR_FATAL;
      }

      IStatus_init(fbc->st);
      IAttachment_execute(fbc->att, fbc->st, fbc->tra, 0, create_buf,
                          FB_DIALECT, NULL, NULL, NULL, NULL);

      if (fb_check_status(fbc->st))
      {
        long sqlcode = p_isc_sqlcode(IStatus_getErrors(fbc->st));
        if (sqlcode == -607)
        {
          if (auto_txn)
          {
            struct IStatus *rst = IMaster_getStatus(fb_master);
            ITransaction_rollback(fbc->tra, rst);
            IStatus_dispose(rst);
            fbc->tra = NULL;
          }
          rs->counter = SB_CNT_OTHER;
          rs->nrows = 0;
          return DB_ERROR_NONE;
        }
        fb_log_error("execute(CREATE IF NOT EXISTS)", fbc->st);
        if (auto_txn) { struct IStatus *rst = IMaster_getStatus(fb_master); ITransaction_rollback(fbc->tra, rst); IStatus_dispose(rst); fbc->tra = NULL; }
        rs->counter = SB_CNT_ERROR;
        return DB_ERROR_FATAL;
      }

      if (auto_txn) { ITransaction_commit(fbc->tra, fbc->st); fbc->tra = NULL; }
      rs->counter = SB_CNT_OTHER;
      rs->nrows = 0;
      return DB_ERROR_NONE;
    }
  }

  /* Handle DROP TABLE IF EXISTS */
  if (strncasecmp(query, "DROP TABLE IF EXISTS ", 21) == 0)
  {
    char drop_buf[256];
    snprintf(drop_buf, sizeof(drop_buf), "DROP TABLE %s", query + 21);

    int auto_txn = (fbc->tra == NULL);
    if (auto_txn && fb_ensure_transaction(fbc))
    {
      rs->counter = SB_CNT_ERROR;
      return DB_ERROR_FATAL;
    }

    IStatus_init(fbc->st);
    IAttachment_execute(fbc->att, fbc->st, fbc->tra, 0, drop_buf,
                        FB_DIALECT, NULL, NULL, NULL, NULL);

    if (fb_check_status(fbc->st))
    {
      long sqlcode = p_isc_sqlcode(IStatus_getErrors(fbc->st));
      if (sqlcode == -607)
      {
        if (auto_txn)
        {
          struct IStatus *rst = IMaster_getStatus(fb_master);
          ITransaction_rollback(fbc->tra, rst);
          IStatus_dispose(rst);
          fbc->tra = NULL;
        }
        rs->counter = SB_CNT_OTHER;
        rs->nrows = 0;
        return DB_ERROR_NONE;
      }
      fb_log_error("execute(DROP IF EXISTS)", fbc->st);
      if (auto_txn) { struct IStatus *rst = IMaster_getStatus(fb_master); ITransaction_rollback(fbc->tra, rst); IStatus_dispose(rst); fbc->tra = NULL; }
      rs->counter = SB_CNT_ERROR;
      return DB_ERROR_FATAL;
    }

    if (auto_txn) { ITransaction_commit(fbc->tra, fbc->st); fbc->tra = NULL; }
    rs->counter = SB_CNT_OTHER;
    rs->nrows = 0;
    return DB_ERROR_NONE;
  }

  /* Batch INSERT optimization — detect "INSERT INTO ... VALUES(...)" */
  if (strncasecmp(query, "INSERT INTO ", 12) == 0)
  {
    const char *vals = strstr(query, "VALUES");
    if (vals == NULL) vals = strstr(query, "values");
    if (vals != NULL)
    {
      unsigned base_len = (unsigned)(vals + 6 - query);
      char base_sql[512];
      if (base_len < sizeof(base_sql))
      {
        memcpy(base_sql, query, base_len);
        base_sql[base_len] = '\0';

        if (fbc->batch == NULL || fbc->batch_base_sql == NULL ||
            strncmp(fbc->batch_base_sql, base_sql, base_len) != 0)
        {
          fb_batch_close(fbc);

          unsigned ncols = 1;
          for (const char *c = vals + 6; *c && *c != ')'; c++)
          {
            if (*c == '\'')
            {
              c++;
              while (*c && !(*c == '\'' && *(c + 1) != '\''))
              {
                if (*c == '\'' && *(c + 1) == '\'') c++;
                c++;
              }
            }
            else if (*c == ',') ncols++;
          }

          fbc->batch_auto_txn = (fbc->tra == NULL);
          if (fb_ensure_transaction(fbc))
          {
            rs->counter = SB_CNT_ERROR;
            return DB_ERROR_FATAL;
          }

          int brc = fb_batch_create(fbc, base_sql, ncols);
          if (brc == 1)
          {
            rs->counter = SB_CNT_ERROR;
            return DB_ERROR_FATAL;
          }
          if (brc == -1)
            goto regular_query;
        }

        if (fbc->batch != NULL && fb_batch_add_row(fbc, vals + 6) == 0)
        {
          rs->counter = SB_CNT_WRITE;
          rs->nrows = 1;
          rs->ptr = NULL;
          return DB_ERROR_NONE;
        }
      }
    }
  }

regular_query:
  /* Regular query via direct execution */
  int auto_txn = (fbc->tra == NULL);
  if (auto_txn && fb_ensure_transaction(fbc))
  {
    rs->counter = SB_CNT_ERROR;
    return DB_ERROR_FATAL;
  }

  /* Try as DML/DDL first via execute */
  IStatus_init(fbc->st);

  struct IStatement *tmp_stmt = IAttachment_prepare(fbc->att, fbc->st,
      fbc->tra, 0, query, FB_DIALECT, IStatement_PREPARE_PREFETCH_METADATA);

  if (fb_check_status(fbc->st))
  {
    db_error_t err = fb_handle_error(sb_conn, fbc, "prepare", query,
                                      &rs->counter);
    if (auto_txn && fbc->tra)
    {
      struct IStatus *rst = IMaster_getStatus(fb_master);
      ITransaction_rollback(fbc->tra, rst);
      IStatus_dispose(rst);
      fbc->tra = NULL;
    }
    return err;
  }

  struct IMessageMetadata *out_meta = IStatement_getOutputMetadata(tmp_stmt,
                                                                    fbc->st);
  unsigned nfields = IMessageMetadata_getCount(out_meta, fbc->st);

  if (nfields > 0)
  {
    unsigned out_len = IMessageMetadata_getMessageLength(out_meta, fbc->st);
    unsigned char *out_buf = calloc(1, out_len);
    if (out_buf == NULL)
    {
      IMessageMetadata_release(out_meta);
      IStatement_free(tmp_stmt, fbc->st);
      rs->counter = SB_CNT_ERROR;
      return DB_ERROR_FATAL;
    }

    rs->counter = SB_CNT_READ;

    IStatus_init(fbc->st);
    struct IResultSet *curs = IStatement_openCursor(tmp_stmt, fbc->st,
        fbc->tra, NULL, NULL, out_meta, 0);

    if (fb_check_status(fbc->st))
    {
      free(out_buf);
      IMessageMetadata_release(out_meta);
      IStatement_free(tmp_stmt, fbc->st);
      if (auto_txn && fbc->tra)
      {
        struct IStatus *rst = IMaster_getStatus(fb_master);
        ITransaction_rollback(fbc->tra, rst);
        IStatus_dispose(rst);
        fbc->tra = NULL;
      }
      return fb_handle_error(sb_conn, fbc, "openCursor", query, &rs->counter);
    }

    fb_result_t *fbrs = (fb_result_t *)calloc(1, sizeof(fb_result_t));
    unsigned capacity = 64;
    unsigned nrows = 0;
    fbrs->nfields = nfields;
    fbrs->values = (db_value_t *)calloc(capacity * nfields, sizeof(db_value_t));

    while (IResultSet_fetchNext(curs, fbc->st, out_buf) == IStatus_RESULT_OK)
    {
      if (nrows >= capacity)
      {
        capacity *= 2;
        db_value_t *nv = realloc(fbrs->values, capacity * nfields * sizeof(db_value_t));
        if (nv == NULL) { fb_free_result(fbrs); free(out_buf); IResultSet_close(curs, fbc->st); IMessageMetadata_release(out_meta); IStatement_free(tmp_stmt, fbc->st); rs->counter = SB_CNT_ERROR; return DB_ERROR_FATAL; }
        fbrs->values = nv;
      }
      for (unsigned ci = 0; ci < nfields; ci++)
      {
        uint32_t col_len = 0;
        char *s = fb_extract_column_alloc(out_meta, fbc->st, out_buf, ci,
                                           &col_len);
        fbrs->values[nrows * nfields + ci].ptr = s;
        fbrs->values[nrows * nfields + ci].len = col_len;
        fb_result_add_string(fbrs, s);
      }
      nrows++;
    }

    fbrs->nrows = nrows;
    rs->nrows = nrows;
    rs->nfields = nfields;
    rs->ptr = fbrs;

    IResultSet_close(curs, fbc->st);
    free(out_buf);
    IMessageMetadata_release(out_meta);
    IStatement_free(tmp_stmt, fbc->st);

    if (auto_txn)
    {
      ITransaction_commit(fbc->tra, fbc->st);
      fbc->tra = NULL;
    }

    return DB_ERROR_NONE;
  }

  /* DML or DDL */
  IMessageMetadata_release(out_meta);

  IStatus_init(fbc->st);
  IStatement_execute(tmp_stmt, fbc->st, fbc->tra, NULL, NULL, NULL, NULL);

  if (fb_check_status(fbc->st))
  {
    IStatement_free(tmp_stmt, fbc->st);
    if (auto_txn && fbc->tra)
    {
      struct IStatus *rst = IMaster_getStatus(fb_master);
      ITransaction_rollback(fbc->tra, rst);
      IStatus_dispose(rst);
      fbc->tra = NULL;
    }
    return fb_handle_error(sb_conn, fbc, "execute", query, &rs->counter);
  }

  IStatement_free(tmp_stmt, fbc->st);

  rs->counter = SB_CNT_WRITE;
  rs->nrows = 1;
  rs->ptr = NULL;

  if (auto_txn)
  {
    ITransaction_commit(fbc->tra, fbc->st);
    fbc->tra = NULL;
  }

  return DB_ERROR_NONE;
}


int firebird_drv_fetch(db_result_t *rs)
{
  (void)rs;
  return 1;
}


int firebird_drv_fetch_row(db_result_t *rs, db_row_t *row)
{
  if (rs->ptr == NULL)
    return DB_ERROR_IGNORABLE;

  intptr_t rownum = (intptr_t)row->ptr;
  if (rownum >= (intptr_t)rs->nrows)
    return DB_ERROR_IGNORABLE;

  if (rs->statement != NULL && rs->statement->emulated == 0 &&
      rs->ptr == rs->statement->ptr)
  {
    fb_stmt_t *fbstmt = (fb_stmt_t *)rs->statement->ptr;
    for (unsigned i = 0; i < fbstmt->nfields; i++)
    {
      row->values[i].len = fbstmt->cached_values[i].len;
      row->values[i].ptr = fbstmt->cached_values[i].ptr;
    }
  }
  else
  {
    fb_result_t *fbrs = (fb_result_t *)rs->ptr;
    db_value_t *src = &fbrs->values[rownum * fbrs->nfields];
    for (unsigned i = 0; i < fbrs->nfields; i++)
    {
      row->values[i].len = src[i].len;
      row->values[i].ptr = src[i].ptr;
    }
  }

  row->ptr = (void *)(rownum + 1);
  return DB_ERROR_NONE;
}


int firebird_drv_free_results(db_result_t *rs)
{
  if (rs->statement != NULL && rs->statement->emulated == 0)
  {
    if (rs->ptr != NULL && rs->ptr != rs->statement->ptr)
    {
      fb_free_result((fb_result_t *)rs->ptr);
    }
    rs->ptr = NULL;
    rs->row.ptr = 0;
    return 0;
  }

  fb_result_t *fbrs = (fb_result_t *)rs->ptr;
  if (fbrs != NULL)
  {
    fb_free_result(fbrs);
    rs->ptr = NULL;
  }

  rs->row.ptr = 0;
  return 0;
}


int firebird_drv_close(db_stmt_t *stmt)
{
  fb_conn_t *fbc = (fb_conn_t *)stmt->connection->ptr;
  fb_stmt_t *fbstmt = (fb_stmt_t *)stmt->ptr;

  if (fbstmt == NULL)
    return 1;

  if (fbstmt->cursor_open && fbstmt->cursor)
  {
    IResultSet_close(fbstmt->cursor, fbc->st);
    fbstmt->cursor = NULL;
    fbstmt->cursor_open = 0;
  }

  if (fbstmt->conv_bufs != NULL)
  {
    for (unsigned i = 0; i < fbstmt->nfields; i++)
      free(fbstmt->conv_bufs[i]);
    free(fbstmt->conv_bufs);
  }
  free(fbstmt->cached_values);
  free(fbstmt->out_buf);
  free(fbstmt->in_buf);

  if (fbstmt->out_meta) IMessageMetadata_release(fbstmt->out_meta);
  if (fbstmt->in_meta) IMessageMetadata_release(fbstmt->in_meta);
  if (fbstmt->stmt) IStatement_free(fbstmt->stmt, fbc->st);

  xfree(stmt->ptr);
  return 0;
}


int firebird_drv_done(void)
{
  if (fb_prov)
  {
    IProvider_release(fb_prov);
    fb_prov = NULL;
  }
  if (fb_dlhandle)
  {
    dlclose(fb_dlhandle);
    fb_dlhandle = NULL;
  }
  return 0;
}
