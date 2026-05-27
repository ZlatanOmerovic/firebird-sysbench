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

#include <ibase.h>

#include "sb_options.h"
#include "db_driver.h"

#define xfree(ptr) ({ if (ptr) free((void *)ptr); ptr = NULL; })

#define MAX_PARAM_LENGTH 256UL
#define MAX_COLUMN_LENGTH 512UL
#define INITIAL_ROW_CAPACITY 64

#define SQL_DIALECT_V6 3

static sb_arg_t firebird_drv_args[] =
{
  SB_OPT("firebird-db", "Firebird database connection string",
         "localhost:/tmp/sbtest.fdb", STRING),
  SB_OPT("firebird-user", "Firebird user", "SYSDBA", STRING),
  SB_OPT("firebird-password", "Firebird password", "masterkey", STRING),

  SB_OPT_END
};

typedef struct
{
  const char *db;
  const char *user;
  const char *password;
} fb_drv_args_t;

typedef struct
{
  isc_db_handle  db;
  isc_tr_handle  trans;
} fb_conn_t;

typedef struct
{
  isc_stmt_handle  stmt;
  XSQLDA           *in_sqlda;
  XSQLDA           *out_sqlda;
  int              nparams;
  char             prepared;
  uint32_t         nfields;
  char             cursor_open;
  db_value_t       *cached_values;
  char             **conv_bufs;
} fb_stmt_t;

typedef struct
{
  isc_stmt_handle  stmt;
  XSQLDA           *out_sqlda;
  char             owns_stmt;
  uint32_t         nrows;
  uint32_t         nfields;
  db_value_t       *values;
  char             **strings;
  uint32_t         nstrings;
  uint32_t         strings_capacity;
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

static void fb_log_error(const char *func, ISC_STATUS_ARRAY status)
{
  char msg[512];
  const ISC_STATUS *p = status;

  log_text(LOG_FATAL, "%s() failed:", func);
  while (fb_interpret(msg, sizeof(msg), &p))
    log_text(LOG_FATAL, "  %s", msg);
}

static int fb_ensure_transaction(fb_conn_t *fbc, ISC_STATUS_ARRAY status)
{
  if (fbc->trans != 0)
    return 0;

  if (isc_start_transaction(status, &fbc->trans, 1, &fbc->db, 0, NULL))
  {
    fb_log_error("isc_start_transaction", status);
    return 1;
  }

  return 0;
}

static XSQLDA *fb_alloc_sqlda(int n)
{
  XSQLDA *sqlda = (XSQLDA *)malloc(XSQLDA_LENGTH(n));
  if (sqlda == NULL)
    return NULL;
  memset(sqlda, 0, XSQLDA_LENGTH(n));
  sqlda->sqln = n;
  sqlda->version = SQLDA_VERSION1;
  return sqlda;
}

static void fb_free_sqlda_buffers(XSQLDA *sqlda)
{
  if (sqlda == NULL)
    return;

  for (int i = 0; i < sqlda->sqld; i++)
  {
    XSQLVAR *var = &sqlda->sqlvar[i];
    xfree(var->sqldata);
    xfree(var->sqlind);
  }
}

static int fb_allocate_output_buffers(XSQLDA *sqlda)
{
  for (int i = 0; i < sqlda->sqld; i++)
  {
    XSQLVAR *var = &sqlda->sqlvar[i];
    short dtype = var->sqltype & ~1;

    switch (dtype)
    {
    case SQL_VARYING:
      var->sqldata = (char *)malloc(var->sqllen + 2);
      break;
    case SQL_TEXT:
      var->sqldata = (char *)malloc(var->sqllen + 1);
      break;
    default:
      var->sqldata = (char *)malloc(var->sqllen);
      break;
    }

    if (var->sqldata == NULL)
      return 1;

    if (var->sqltype & 1)
    {
      var->sqlind = (short *)malloc(sizeof(short));
      if (var->sqlind == NULL)
        return 1;
      *var->sqlind = 0;
    }
  }

  return 0;
}

static void fb_extract_column_fast(XSQLVAR *var, db_value_t *val, char *conv_buf)
{
  if ((var->sqltype & 1) && var->sqlind && *var->sqlind == -1)
  {
    val->ptr = NULL;
    val->len = 0;
    return;
  }

  short dtype = var->sqltype & ~1;

  switch (dtype)
  {
  case SQL_TEXT:
  {
    int len = var->sqllen;
    while (len > 0 && var->sqldata[len - 1] == ' ')
      len--;
    val->ptr = var->sqldata;
    val->len = (uint32_t)len;
    return;
  }
  case SQL_VARYING:
  {
    ISC_USHORT vary_len;
    memcpy(&vary_len, var->sqldata, sizeof(ISC_USHORT));
    val->ptr = var->sqldata + sizeof(ISC_USHORT);
    val->len = vary_len;
    return;
  }
  case SQL_SHORT:
  {
    short v;
    memcpy(&v, var->sqldata, sizeof(short));
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "%d", (int)v);
    val->ptr = conv_buf;
    return;
  }
  case SQL_LONG:
  {
    ISC_LONG v;
    memcpy(&v, var->sqldata, sizeof(ISC_LONG));
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "%d", (int)v);
    val->ptr = conv_buf;
    return;
  }
  case SQL_INT64:
  {
    ISC_INT64 v;
    memcpy(&v, var->sqldata, sizeof(ISC_INT64));
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "%" PRId64, (int64_t)v);
    val->ptr = conv_buf;
    return;
  }
  case SQL_FLOAT:
  {
    float v;
    memcpy(&v, var->sqldata, sizeof(float));
    val->len = (uint32_t)snprintf(conv_buf, MAX_COLUMN_LENGTH, "%g", (double)v);
    val->ptr = conv_buf;
    return;
  }
  case SQL_DOUBLE:
  {
    double v;
    memcpy(&v, var->sqldata, sizeof(double));
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

static char *fb_extract_column(XSQLVAR *var, uint32_t *out_len)
{
  if ((var->sqltype & 1) && var->sqlind && *var->sqlind == -1)
  {
    *out_len = 0;
    return NULL;
  }

  char buf[MAX_COLUMN_LENGTH];
  int n;
  short dtype = var->sqltype & ~1;

  switch (dtype)
  {
  case SQL_TEXT:
  {
    int len = var->sqllen;
    while (len > 0 && var->sqldata[len - 1] == ' ')
      len--;
    *out_len = (uint32_t)len;
    char *s = malloc(len + 1);
    if (s == NULL)
      return NULL;
    memcpy(s, var->sqldata, len);
    s[len] = '\0';
    return s;
  }
  case SQL_VARYING:
  {
    ISC_USHORT vary_len;
    memcpy(&vary_len, var->sqldata, sizeof(ISC_USHORT));
    *out_len = vary_len;
    char *s = malloc(vary_len + 1);
    if (s == NULL)
      return NULL;
    memcpy(s, var->sqldata + sizeof(ISC_USHORT), vary_len);
    s[vary_len] = '\0';
    return s;
  }
  case SQL_SHORT:
  {
    short val;
    memcpy(&val, var->sqldata, sizeof(short));
    n = snprintf(buf, sizeof(buf), "%d", (int)val);
    break;
  }
  case SQL_LONG:
  {
    ISC_LONG val;
    memcpy(&val, var->sqldata, sizeof(ISC_LONG));
    n = snprintf(buf, sizeof(buf), "%d", (int)val);
    break;
  }
  case SQL_INT64:
  {
    ISC_INT64 val;
    memcpy(&val, var->sqldata, sizeof(ISC_INT64));
    n = snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)val);
    break;
  }
  case SQL_FLOAT:
  {
    float val;
    memcpy(&val, var->sqldata, sizeof(float));
    n = snprintf(buf, sizeof(buf), "%g", (double)val);
    break;
  }
  case SQL_DOUBLE:
  {
    double val;
    memcpy(&val, var->sqldata, sizeof(double));
    n = snprintf(buf, sizeof(buf), "%g", val);
    break;
  }
  default:
    n = snprintf(buf, sizeof(buf), "?");
    break;
  }

  if (n < 0)
    n = 0;

  *out_len = (uint32_t)n;
  char *s = malloc(n + 1);
  if (s == NULL)
    return NULL;
  memcpy(s, buf, n);
  s[n] = '\0';
  return s;
}

static void fb_result_add_string(fb_result_t *fbrs, char *s)
{
  if (s == NULL)
    return;

  if (fbrs->nstrings >= fbrs->strings_capacity)
  {
    uint32_t new_cap = fbrs->strings_capacity ? fbrs->strings_capacity * 2 : 64;
    char **new_arr = realloc(fbrs->strings, new_cap * sizeof(char *));
    if (new_arr == NULL)
    {
      free(s);
      return;
    }
    fbrs->strings = new_arr;
    fbrs->strings_capacity = new_cap;
  }

  fbrs->strings[fbrs->nstrings++] = s;
}

static fb_result_t *fb_fetch_all_rows(XSQLDA *out_sqlda,
                                      isc_stmt_handle *stmt_handle,
                                      ISC_STATUS_ARRAY status,
                                      uint32_t *total_rows)
{
  uint32_t nfields = (uint32_t)out_sqlda->sqld;
  uint32_t capacity = INITIAL_ROW_CAPACITY;
  uint32_t nrows = 0;

  fb_result_t *fbrs = (fb_result_t *)calloc(1, sizeof(fb_result_t));
  if (fbrs == NULL)
    return NULL;

  fbrs->nfields = nfields;
  fbrs->values = (db_value_t *)calloc(capacity * nfields, sizeof(db_value_t));
  if (fbrs->values == NULL)
  {
    free(fbrs);
    return NULL;
  }

  ISC_STATUS fetch_stat;
  while ((fetch_stat = isc_dsql_fetch(status, stmt_handle, SQL_DIALECT_V6,
                                       out_sqlda)) == 0)
  {
    if (nrows >= capacity)
    {
      capacity *= 2;
      db_value_t *new_vals = realloc(fbrs->values,
                                     capacity * nfields * sizeof(db_value_t));
      if (new_vals == NULL)
        break;
      fbrs->values = new_vals;
    }

    for (uint32_t i = 0; i < nfields; i++)
    {
      uint32_t col_len = 0;
      char *s = fb_extract_column(&out_sqlda->sqlvar[i], &col_len);

      db_value_t *v = &fbrs->values[nrows * nfields + i];
      v->ptr = s;
      v->len = col_len;

      fb_result_add_string(fbrs, s);
    }
    nrows++;
  }

  fbrs->nrows = nrows;
  *total_rows = nrows;

  if (fetch_stat != 100 && fetch_stat != 0)
    return fbrs;

  return fbrs;
}

static void fb_free_result(fb_result_t *fbrs)
{
  ISC_STATUS_ARRAY status;

  if (fbrs == NULL)
    return;

  for (uint32_t i = 0; i < fbrs->nstrings; i++)
    free(fbrs->strings[i]);
  free(fbrs->strings);
  free(fbrs->values);

  if (fbrs->out_sqlda != NULL)
  {
    fb_free_sqlda_buffers(fbrs->out_sqlda);
    free(fbrs->out_sqlda);
  }
  if (fbrs->owns_stmt && fbrs->stmt != 0)
    isc_dsql_free_statement(status, &fbrs->stmt, DSQL_drop);

  free(fbrs);
}

static db_error_t fb_check_error(db_conn_t *con, ISC_STATUS_ARRAY status,
                                 const char *func, const char *query,
                                 sb_counter_type_t *counter)
{
  long sqlcode = isc_sqlcode(status);

  char msg[512];
  const ISC_STATUS *p = status;
  msg[0] = '\0';
  fb_interpret(msg, sizeof(msg), &p);

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
    fb_conn_t *fbc = (fb_conn_t *)con->ptr;
    if (fbc->trans != 0)
    {
      ISC_STATUS_ARRAY rb_status;
      isc_rollback_transaction(rb_status, &fbc->trans);
      fbc->trans = 0;
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
  ISC_STATUS_ARRAY status;
  fb_conn_t *fbc;
  char dpb[256];
  char *p;
  size_t len;

  fbc = (fb_conn_t *)calloc(1, sizeof(fb_conn_t));
  if (fbc == NULL)
    return 1;

  p = dpb;
  *p++ = isc_dpb_version1;

  *p++ = isc_dpb_user_name;
  len = strlen(args.user);
  *p++ = (char)len;
  memcpy(p, args.user, len);
  p += len;

  *p++ = isc_dpb_password;
  len = strlen(args.password);
  *p++ = (char)len;
  memcpy(p, args.password, len);
  p += len;

  *p++ = isc_dpb_lc_ctype;
  len = 4;
  *p++ = (char)len;
  memcpy(p, "UTF8", len);
  p += len;

  short dpb_len = (short)(p - dpb);

  if (isc_attach_database(status, 0, args.db, &fbc->db, dpb_len, dpb))
  {
    fb_log_error("isc_attach_database", status);
    free(fbc);
    return 1;
  }

  fbc->trans = 0;
  sb_conn->ptr = fbc;

  return 0;
}


int firebird_drv_disconnect(db_conn_t *sb_conn)
{
  ISC_STATUS_ARRAY status;
  fb_conn_t *fbc = (fb_conn_t *)sb_conn->ptr;

  xfree(sb_conn->sql_state);
  xfree(sb_conn->sql_errmsg);

  if (fbc == NULL)
    return 0;

  if (fbc->trans != 0)
  {
    isc_rollback_transaction(status, &fbc->trans);
    fbc->trans = 0;
  }

  if (fbc->db != 0)
  {
    isc_detach_database(status, &fbc->db);
    fbc->db = 0;
  }

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
  ISC_STATUS_ARRAY status;
  fb_conn_t *fbc = (fb_conn_t *)stmt->connection->ptr;
  fb_stmt_t *fbstmt = NULL;
  int rc = 1;
  int n;

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

  if (fb_ensure_transaction(fbc, status))
    goto error;

  if (isc_dsql_allocate_statement(status, &fbc->db, &fbstmt->stmt))
  {
    fb_log_error("isc_dsql_allocate_statement", status);
    goto error;
  }

  fbstmt->out_sqlda = fb_alloc_sqlda(20);
  if (fbstmt->out_sqlda == NULL)
    goto error;

  if (isc_dsql_prepare(status, &fbc->trans, &fbstmt->stmt, 0, query,
                        SQL_DIALECT_V6, fbstmt->out_sqlda))
  {
    fb_log_error("isc_dsql_prepare", status);
    goto error;
  }

  if (fbstmt->out_sqlda->sqld > fbstmt->out_sqlda->sqln)
  {
    n = fbstmt->out_sqlda->sqld;
    free(fbstmt->out_sqlda);
    fbstmt->out_sqlda = fb_alloc_sqlda(n);
    if (fbstmt->out_sqlda == NULL)
      goto error;
    if (isc_dsql_describe(status, &fbstmt->stmt, SQL_DIALECT_V6,
                          fbstmt->out_sqlda))
    {
      fb_log_error("isc_dsql_describe", status);
      goto error;
    }
  }

  if (fbstmt->out_sqlda->sqld > 0)
  {
    if (fb_allocate_output_buffers(fbstmt->out_sqlda))
      goto error;
  }

  fbstmt->in_sqlda = fb_alloc_sqlda(20);
  if (fbstmt->in_sqlda == NULL)
    goto error;

  if (isc_dsql_describe_bind(status, &fbstmt->stmt, SQL_DIALECT_V6,
                              fbstmt->in_sqlda))
  {
    fb_log_error("isc_dsql_describe_bind", status);
    goto error;
  }

  if (fbstmt->in_sqlda->sqld > fbstmt->in_sqlda->sqln)
  {
    n = fbstmt->in_sqlda->sqld;
    free(fbstmt->in_sqlda);
    fbstmt->in_sqlda = fb_alloc_sqlda(n);
    if (fbstmt->in_sqlda == NULL)
      goto error;
    if (isc_dsql_describe_bind(status, &fbstmt->stmt, SQL_DIALECT_V6,
                                fbstmt->in_sqlda))
    {
      fb_log_error("isc_dsql_describe_bind", status);
      goto error;
    }
  }

  fbstmt->nparams = fbstmt->in_sqlda->sqld;
  fbstmt->nfields = (uint32_t)fbstmt->out_sqlda->sqld;
  fbstmt->prepared = 1;

  if (fbstmt->nfields > 0)
  {
    fbstmt->cached_values = (db_value_t *)calloc(fbstmt->nfields, sizeof(db_value_t));
    fbstmt->conv_bufs = (char **)calloc(fbstmt->nfields, sizeof(char *));
    if (fbstmt->cached_values == NULL || fbstmt->conv_bufs == NULL)
      goto error;
    for (uint32_t i = 0; i < fbstmt->nfields; i++)
    {
      fbstmt->conv_bufs[i] = (char *)malloc(MAX_COLUMN_LENGTH);
      if (fbstmt->conv_bufs[i] == NULL)
        goto error;
    }
  }

  for (int i = 0; i < fbstmt->nparams; i++)
  {
    XSQLVAR *var = &fbstmt->in_sqlda->sqlvar[i];
    var->sqldata = (char *)calloc(1, MAX_PARAM_LENGTH + 2);
    if (var->sqldata == NULL)
      goto error;
    var->sqlind = (short *)malloc(sizeof(short));
    if (var->sqlind == NULL)
      goto error;
    *var->sqlind = 0;
  }

  stmt->ptr = fbstmt;
  stmt->query = strdup(query);

  return 0;

error:
  if (fbstmt != NULL)
  {
    if (fbstmt->in_sqlda != NULL)
    {
      for (int i = 0; i < fbstmt->in_sqlda->sqld; i++)
      {
        xfree(fbstmt->in_sqlda->sqlvar[i].sqldata);
        xfree(fbstmt->in_sqlda->sqlvar[i].sqlind);
      }
      free(fbstmt->in_sqlda);
    }
    if (fbstmt->conv_bufs != NULL)
    {
      for (uint32_t i = 0; i < fbstmt->nfields; i++)
        if (fbstmt->conv_bufs[i]) free(fbstmt->conv_bufs[i]);
      free(fbstmt->conv_bufs);
    }
    free(fbstmt->cached_values);
    if (fbstmt->out_sqlda != NULL)
    {
      fb_free_sqlda_buffers(fbstmt->out_sqlda);
      free(fbstmt->out_sqlda);
    }
    if (fbstmt->stmt != 0)
      isc_dsql_free_statement(status, &fbstmt->stmt, DSQL_drop);
    free(fbstmt);
  }

  return rc;
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

  if ((unsigned)fbstmt->nparams != len)
  {
    log_text(LOG_ALERT, "wrong number of parameters in prepared statement");
    return 1;
  }

  return 0;
}


int firebird_drv_bind_result(db_stmt_t *stmt, db_bind_t *params, size_t len)
{
  (void)stmt;
  (void)params;
  (void)len;
  return 0;
}


static void fb_set_param(XSQLVAR *var, db_bind_t *bind)
{
  if (bind->is_null && *bind->is_null)
  {
    *var->sqlind = -1;
    return;
  }

  *var->sqlind = 0;

  switch (bind->type)
  {
  case DB_TYPE_TINYINT:
  case DB_TYPE_SMALLINT:
  {
    short val;
    if (bind->type == DB_TYPE_TINYINT)
      val = (short)(*(char *)bind->buffer);
    else
      val = *(short *)bind->buffer;
    var->sqltype = SQL_SHORT + 1;
    var->sqllen = sizeof(short);
    memcpy(var->sqldata, &val, sizeof(short));
    break;
  }
  case DB_TYPE_INT:
  {
    ISC_LONG val = *(int *)bind->buffer;
    var->sqltype = SQL_LONG + 1;
    var->sqllen = sizeof(ISC_LONG);
    memcpy(var->sqldata, &val, sizeof(ISC_LONG));
    break;
  }
  case DB_TYPE_BIGINT:
  {
    ISC_INT64 val = *(long long *)bind->buffer;
    var->sqltype = SQL_INT64 + 1;
    var->sqllen = sizeof(ISC_INT64);
    memcpy(var->sqldata, &val, sizeof(ISC_INT64));
    break;
  }
  case DB_TYPE_FLOAT:
  {
    float val = *(float *)bind->buffer;
    var->sqltype = SQL_FLOAT + 1;
    var->sqllen = sizeof(float);
    memcpy(var->sqldata, &val, sizeof(float));
    break;
  }
  case DB_TYPE_DOUBLE:
  {
    double val = *(double *)bind->buffer;
    var->sqltype = SQL_DOUBLE + 1;
    var->sqllen = sizeof(double);
    memcpy(var->sqldata, &val, sizeof(double));
    break;
  }
  case DB_TYPE_CHAR:
  case DB_TYPE_VARCHAR:
  {
    unsigned long data_len = bind->data_len
      ? *bind->data_len
      : strlen((char *)bind->buffer);
    if (data_len > MAX_PARAM_LENGTH)
      data_len = MAX_PARAM_LENGTH;

    ISC_USHORT vary_len = (ISC_USHORT)data_len;
    memcpy(var->sqldata, &vary_len, sizeof(ISC_USHORT));
    memcpy(var->sqldata + sizeof(ISC_USHORT), bind->buffer, data_len);

    var->sqltype = SQL_VARYING + 1;
    var->sqllen = (short)data_len;
    break;
  }
  default:
  {
    char buf[MAX_PARAM_LENGTH];
    int n = db_print_value(bind, buf, sizeof(buf));
    if (n > 0)
    {
      ISC_USHORT vary_len = (ISC_USHORT)n;
      memcpy(var->sqldata, &vary_len, sizeof(ISC_USHORT));
      memcpy(var->sqldata + sizeof(ISC_USHORT), buf, n);
      var->sqltype = SQL_VARYING + 1;
      var->sqllen = (short)n;
    }
    break;
  }
  }
}


db_error_t firebird_drv_execute(db_stmt_t *stmt, db_result_t *rs)
{
  ISC_STATUS_ARRAY status;
  db_conn_t *con = stmt->connection;
  fb_conn_t *fbc = (fb_conn_t *)con->ptr;
  fb_stmt_t *fbstmt;
  char *buf = NULL;
  unsigned int buflen = 0;
  unsigned int i, j, vcnt;
  char need_realloc;
  int n;

  con->sql_errno = 0;
  con->sql_state = NULL;
  con->sql_errmsg = NULL;

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
      isc_dsql_free_statement(status, &fbstmt->stmt, DSQL_close);
      fbstmt->cursor_open = 0;
    }

    for (i = 0; i < (unsigned)fbstmt->nparams; i++)
      fb_set_param(&fbstmt->in_sqlda->sqlvar[i], &stmt->bound_param[i]);

    if (fb_ensure_transaction(fbc, status))
      return DB_ERROR_FATAL;

    if (isc_dsql_execute(status, &fbc->trans, &fbstmt->stmt, SQL_DIALECT_V6,
                          fbstmt->nparams > 0 ? fbstmt->in_sqlda : NULL))
      return fb_check_error(con, status, "isc_dsql_execute", stmt->query,
                            &rs->counter);

    if (fbstmt->nfields > 0)
    {
      rs->counter = SB_CNT_READ;

      ISC_STATUS fetch_stat;
      uint32_t row_count = 0;

      fetch_stat = isc_dsql_fetch(status, &fbstmt->stmt, SQL_DIALECT_V6,
                                   fbstmt->out_sqlda);

      if (fetch_stat == 0)
      {
        for (uint32_t ci = 0; ci < fbstmt->nfields; ci++)
          fb_extract_column_fast(&fbstmt->out_sqlda->sqlvar[ci],
                                 &fbstmt->cached_values[ci],
                                 fbstmt->conv_bufs[ci]);
        row_count = 1;

        while ((fetch_stat = isc_dsql_fetch(status, &fbstmt->stmt,
                                             SQL_DIALECT_V6,
                                             fbstmt->out_sqlda)) == 0)
          row_count++;
      }

      if (fetch_stat != 100 && fetch_stat != 0)
        return fb_check_error(con, status, "isc_dsql_fetch", stmt->query,
                              &rs->counter);

      fbstmt->cursor_open = 1;
      rs->nrows = row_count;
      rs->nfields = fbstmt->nfields;
      rs->ptr = fbstmt;

      return DB_ERROR_NONE;
    }

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
  ISC_STATUS_ARRAY status;
  fb_conn_t *fbc = (fb_conn_t *)sb_conn->ptr;

  (void)len;

  sb_conn->sql_errno = 0;
  sb_conn->sql_state = NULL;
  sb_conn->sql_errmsg = NULL;

  /* Intercept transaction control statements */
  if (strcasecmp(query, "BEGIN") == 0)
  {
    if (fbc->trans != 0)
    {
      isc_commit_transaction(status, &fbc->trans);
      fbc->trans = 0;
    }
    if (isc_start_transaction(status, &fbc->trans, 1, &fbc->db, 0, NULL))
    {
      fb_log_error("isc_start_transaction", status);
      rs->counter = SB_CNT_ERROR;
      return DB_ERROR_FATAL;
    }
    rs->counter = SB_CNT_OTHER;
    rs->nrows = 0;
    return DB_ERROR_NONE;
  }

  if (strcasecmp(query, "COMMIT") == 0)
  {
    if (fbc->trans != 0)
    {
      if (isc_commit_transaction(status, &fbc->trans))
      {
        fb_log_error("isc_commit_transaction", status);
        fbc->trans = 0;
        rs->counter = SB_CNT_ERROR;
        return DB_ERROR_FATAL;
      }
      fbc->trans = 0;
    }
    rs->counter = SB_CNT_OTHER;
    rs->nrows = 0;
    return DB_ERROR_NONE;
  }

  if (strcasecmp(query, "ROLLBACK") == 0)
  {
    if (fbc->trans != 0)
    {
      isc_rollback_transaction(status, &fbc->trans);
      fbc->trans = 0;
    }
    rs->counter = SB_CNT_OTHER;
    rs->nrows = 0;
    return DB_ERROR_NONE;
  }

  /* Handle CREATE TABLE IF NOT EXISTS — strip IF NOT EXISTS, ignore -607 */
  {
    const char *q = query;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
      q++;
    if (strncasecmp(q, "CREATE TABLE IF NOT EXISTS ", 27) == 0)
    {
      char create_buf[4096];
      snprintf(create_buf, sizeof(create_buf), "CREATE TABLE %s", q + 27);

    int auto_txn_create = (fbc->trans == 0);
    if (auto_txn_create && fb_ensure_transaction(fbc, status))
    {
      rs->counter = SB_CNT_ERROR;
      return DB_ERROR_FATAL;
    }

    if (isc_dsql_execute_immediate(status, &fbc->db, &fbc->trans, 0,
                                    create_buf, SQL_DIALECT_V6, NULL))
    {
      long sqlcode = isc_sqlcode(status);
      if (sqlcode == -607)
      {
        ISC_STATUS_ARRAY rb_status;
        isc_rollback_transaction(rb_status, &fbc->trans);
        fbc->trans = 0;
        rs->counter = SB_CNT_OTHER;
        rs->nrows = 0;
        return DB_ERROR_NONE;
      }
      fb_log_error("isc_dsql_execute_immediate", status);
      if (auto_txn_create)
      {
        ISC_STATUS_ARRAY rb_status;
        isc_rollback_transaction(rb_status, &fbc->trans);
        fbc->trans = 0;
      }
      rs->counter = SB_CNT_ERROR;
      return DB_ERROR_FATAL;
    }

    if (auto_txn_create)
    {
      isc_commit_transaction(status, &fbc->trans);
      fbc->trans = 0;
    }

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

    int auto_txn_drop = (fbc->trans == 0);
    if (auto_txn_drop && fb_ensure_transaction(fbc, status))
    {
      rs->counter = SB_CNT_ERROR;
      return DB_ERROR_FATAL;
    }

    if (isc_dsql_execute_immediate(status, &fbc->db, &fbc->trans, 0,
                                    drop_buf, SQL_DIALECT_V6, NULL))
    {
      long sqlcode = isc_sqlcode(status);
      if (sqlcode == -607)
      {
        ISC_STATUS_ARRAY rb_status;
        isc_rollback_transaction(rb_status, &fbc->trans);
        fbc->trans = 0;
        rs->counter = SB_CNT_OTHER;
        rs->nrows = 0;
        return DB_ERROR_NONE;
      }
      fb_log_error("isc_dsql_execute_immediate", status);
      if (auto_txn_drop)
      {
        ISC_STATUS_ARRAY rb_status;
        isc_rollback_transaction(rb_status, &fbc->trans);
        fbc->trans = 0;
      }
      rs->counter = SB_CNT_ERROR;
      return DB_ERROR_FATAL;
    }

    if (auto_txn_drop)
    {
      isc_commit_transaction(status, &fbc->trans);
      fbc->trans = 0;
    }

    rs->counter = SB_CNT_OTHER;
    rs->nrows = 0;
    return DB_ERROR_NONE;
  }

  /* Regular query */
  int auto_txn = (fbc->trans == 0);
  if (auto_txn && fb_ensure_transaction(fbc, status))
  {
    rs->counter = SB_CNT_ERROR;
    return DB_ERROR_FATAL;
  }

  isc_stmt_handle tmp_stmt = 0;

  if (isc_dsql_allocate_statement(status, &fbc->db, &tmp_stmt))
  {
    fb_log_error("isc_dsql_allocate_statement", status);
    rs->counter = SB_CNT_ERROR;
    return DB_ERROR_FATAL;
  }

  XSQLDA *out_sqlda = fb_alloc_sqlda(20);
  if (out_sqlda == NULL)
    goto query_error;

  if (isc_dsql_prepare(status, &fbc->trans, &tmp_stmt, 0, query,
                        SQL_DIALECT_V6, out_sqlda))
  {
    ISC_STATUS_ARRAY saved_status;
    memcpy(saved_status, status, sizeof(ISC_STATUS_ARRAY));

    free(out_sqlda);
    ISC_STATUS_ARRAY cleanup_status;
    isc_dsql_free_statement(cleanup_status, &tmp_stmt, DSQL_drop);
    if (auto_txn)
    {
      isc_rollback_transaction(cleanup_status, &fbc->trans);
      fbc->trans = 0;
    }
    return fb_check_error(sb_conn, saved_status, "isc_dsql_prepare", query,
                          &rs->counter);
  }

  if (out_sqlda->sqld > out_sqlda->sqln)
  {
    int n = out_sqlda->sqld;
    free(out_sqlda);
    out_sqlda = fb_alloc_sqlda(n);
    if (out_sqlda == NULL)
      goto query_error;
    isc_dsql_describe(status, &tmp_stmt, SQL_DIALECT_V6, out_sqlda);
  }

  if (out_sqlda->sqld > 0)
    fb_allocate_output_buffers(out_sqlda);

  if (isc_dsql_execute(status, &fbc->trans, &tmp_stmt, SQL_DIALECT_V6, NULL))
  {
    ISC_STATUS_ARRAY saved_status;
    memcpy(saved_status, status, sizeof(ISC_STATUS_ARRAY));

    fb_free_sqlda_buffers(out_sqlda);
    free(out_sqlda);
    ISC_STATUS_ARRAY cleanup_status;
    isc_dsql_free_statement(cleanup_status, &tmp_stmt, DSQL_drop);
    if (auto_txn)
    {
      isc_rollback_transaction(cleanup_status, &fbc->trans);
      fbc->trans = 0;
    }
    return fb_check_error(sb_conn, saved_status, "isc_dsql_execute", query,
                          &rs->counter);
  }

  if (out_sqlda->sqld > 0)
  {
    rs->counter = SB_CNT_READ;

    uint32_t total_rows = 0;
    fb_result_t *fbrs = fb_fetch_all_rows(out_sqlda, &tmp_stmt, status,
                                           &total_rows);
    if (fbrs == NULL)
      goto query_error;

    fbrs->stmt = tmp_stmt;
    fbrs->out_sqlda = out_sqlda;
    fbrs->owns_stmt = 1;

    rs->nrows = total_rows;
    rs->nfields = fbrs->nfields;
    rs->ptr = fbrs;

    isc_dsql_free_statement(status, &tmp_stmt, DSQL_close);

    if (auto_txn)
    {
      isc_commit_transaction(status, &fbc->trans);
      fbc->trans = 0;
    }

    return DB_ERROR_NONE;
  }

  /* DML or DDL */
  rs->counter = SB_CNT_WRITE;
  rs->nrows = 1;
  rs->ptr = NULL;

  fb_free_sqlda_buffers(out_sqlda);
  free(out_sqlda);
  isc_dsql_free_statement(status, &tmp_stmt, DSQL_drop);

  if (auto_txn)
  {
    isc_commit_transaction(status, &fbc->trans);
    fbc->trans = 0;
  }

  return DB_ERROR_NONE;

query_error:
  fb_free_sqlda_buffers(out_sqlda);
  free(out_sqlda);
  isc_dsql_free_statement(status, &tmp_stmt, DSQL_drop);
  rs->counter = SB_CNT_ERROR;
  return DB_ERROR_FATAL;
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

  if (rs->statement != NULL && rs->statement->emulated == 0)
  {
    fb_stmt_t *fbstmt = (fb_stmt_t *)rs->statement->ptr;
    for (uint32_t i = 0; i < fbstmt->nfields; i++)
    {
      row->values[i].len = fbstmt->cached_values[i].len;
      row->values[i].ptr = fbstmt->cached_values[i].ptr;
    }
  }
  else
  {
    fb_result_t *fbrs = (fb_result_t *)rs->ptr;
    db_value_t *src = &fbrs->values[rownum * fbrs->nfields];
    for (uint32_t i = 0; i < fbrs->nfields; i++)
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
  ISC_STATUS_ARRAY status;
  fb_stmt_t *fbstmt = (fb_stmt_t *)stmt->ptr;

  if (fbstmt == NULL)
    return 1;

  if (fbstmt->in_sqlda != NULL)
  {
    for (int i = 0; i < fbstmt->nparams; i++)
    {
      xfree(fbstmt->in_sqlda->sqlvar[i].sqldata);
      xfree(fbstmt->in_sqlda->sqlvar[i].sqlind);
    }
    free(fbstmt->in_sqlda);
  }

  if (fbstmt->conv_bufs != NULL)
  {
    for (uint32_t i = 0; i < fbstmt->nfields; i++)
      free(fbstmt->conv_bufs[i]);
    free(fbstmt->conv_bufs);
  }
  free(fbstmt->cached_values);

  if (fbstmt->out_sqlda != NULL)
  {
    fb_free_sqlda_buffers(fbstmt->out_sqlda);
    free(fbstmt->out_sqlda);
  }

  if (fbstmt->stmt != 0)
    isc_dsql_free_statement(status, &fbstmt->stmt, DSQL_drop);

  xfree(stmt->ptr);

  return 0;
}


int firebird_drv_done(void)
{
  return 0;
}
