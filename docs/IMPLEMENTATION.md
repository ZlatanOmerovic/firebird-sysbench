# Implementation Details

## What Was Changed in sysbench

### New files (Firebird-specific)

| File | Lines | Purpose |
|---|---|---|
| `src/drivers/firebird/drv_firebird.c` | 1,894 | Full Firebird OO API driver |
| `src/drivers/firebird/Makefile.am` | 4 | Build file for the driver |
| `m4/ac_check_firebird.m4` | 88 | Autoconf macro locating the Firebird headers |
| `tests/include/firebird_common.sh` | 44 | Test helper: connection setup, db_show_table() |
| `tests/t/drv_firebird.t` | 52 | Driver connectivity test |
| `tests/t/help_drv_firebird.t` | 13 | Help output test |
| `tests/t/api_sql_firebird.t` | 228 | SQL API test: types, bulk insert, NULLs, reconnect |
| `tests/t/script_oltp_point_select_firebird.t` | 55 | Point select integration test |
| `tests/t/script_oltp_read_only_firebird.t` | 60 | Read-only integration test |
| `tests/t/script_oltp_read_write_firebird.t` | 73 | Read/write integration test |
| `tests/t/script_oltp_insert_firebird.t` | 72 | Insert integration test |
| `tests/t/script_oltp_delete_firebird.t` | 60 | Delete integration test |
| `tests/t/script_oltp_general_firebird.t` | 63 | Table structure verification test |
| `tests/t/script_bulk_insert_firebird.t` | 21 | Bulk insert integration test |
| `tests/t/script_select_random_firebird.t` | 39 | Random select integration test |
| `run_benchmarks.sh` | 118 | Full benchmark suite runner |
| `run_benchmarks_readonly.sh` | 77 | Read-safe benchmark suite runner |

### Modified upstream files (additive only)

| File | Change | Lines |
|---|---|---|
| `configure.ac` | `--with-firebird` option, AC_CHECK_FIREBIRD call, config output | +23 |
| `src/db_driver.c` | Driver registration, single-row bulk insert flush, DB_ERROR_IGNORABLE fix | +13 / -3 |
| `src/db_driver.h` | `register_driver_firebird()` prototype, `bulk_single_row` field | +7 / -1 |
| `src/Makefile.am` | `firebird_ldadd` link block | +6 / -2 |
| `src/drivers/Makefile.am` | `FIREBIRD_DIR` conditional | +5 / -1 |
| `src/lua/oltp_common.lua` | Firebird DDL branch in `create_table()` | +7 |
| `src/lua/oltp_insert.lua` | Added `firebird` to the pgsql auto-inc branch | +1 / -1 |
| `tests/include/config.sh.in` | `SBTEST_HAS_FIREBIRD` variable | +1 |
| `.github/workflows/ci.yml` | Firebird CI job, updated Ubuntu versions | +60 / -3 |

`README.md` was rewritten for the fork. No other upstream file is touched.

## Driver Architecture

The driver targets the **Firebird OO API** through the C wrapper
`firebird/fb_c_api.h`, not the legacy ISC API. The only ISC-era symbol still
used is `isc_sqlcode()`, to turn an `IStatus` error chain into the SQLCODE
that sysbench's error classification needs.

### Runtime library loading

`libfbclient.so` is never linked into the binary. `firebird_drv_init()`
`dlopen`s it (path from `--firebird-client`, default `libfbclient.so`
resolved through the normal loader search path) and `dlsym`s exactly two
entry points: `fb_get_master_interface` and `isc_sqlcode`. Everything else
is reached through OO API vtables. `FIREBIRD_LIBS` is therefore just `-ldl`.

Consequences:

- One sysbench binary can benchmark several Firebird versions — swap
  `--firebird-client`.
- A missing or wrong library produces a real error message instead of a link
  failure at build time.
- The client's ABI has to be checked at runtime (below), because the
  compile-time header no longer guarantees what the loaded library provides.

### Client ABI probe

`drv_init` reads the `IUtil` vtable version — the cloop ABI keeps
`vtable->version` at a fixed offset that is safe to read without dispatching
through the vtable:

| Reported vtable | Behaviour | Clients seen here |
|---|---|---|
| below v3 | Fatal error; message points at the `firebird-isc` branch | FB3 reports v2 |
| below v4 | Accepted, `fb_no_batch` set — `IStatement::createBatch` does not exist that far back, so bulk inserts fall back to single-row | none |
| v4 and up | Everything enabled | FB4, FB5, FB6 |

The `fb_no_batch` branch is a safety net, not a version statement. FB4.0.7's
client library already reports v4 or newer — running the suite with
`--firebird-client=/opt/firebird4/lib/libfbclient.so` at `--verbosity=5`
prints no batch-disabled notice — so batching stays on with an FB4 client.

Without this probe, dispatching a method that doesn't exist in an older
vtable reads past its end and crashes.

### Embedded-mode detection

A connection string with no `host:` or `host/port:` prefix makes libfbclient
load the embedded engine in-process. The embedded engine builds a deep object
hierarchy on the calling thread's stack during `attachDatabase` and segfaults
with sysbench's 64K default thread stack. `drv_init` detects the embedded
syntax and warns when `--thread-stack-size` is below 2M.

### Per-process vs per-thread objects

| Object | Scope | Created in |
|---|---|---|
| `IMaster`, `IProvider`, `IUtil` | Per process | `drv_init` |
| `IStatus` | Per connection (not thread-safe) | `connect` |
| `IAttachment` | Per connection | `connect` |
| `ITransaction` | Per connection: `tra` (explicit or write), `tra_read` and `tra_auto` | on demand |

### Connection lifecycle

```
connect:     IUtil_getXpbBuilder(DPB) → user/password/lc_ctype=UTF8
             → IProvider_attachDatabase()
             → first connection of a `run`: one line of server settings
disconnect:  flush+close any IBatch → commit tra_read and tra_auto → rollback tra
             → (--firebird-stats, last connection) print the counters
             → IAttachment_detach() → IStatus_dispose()
reconnect:   disconnect() + connect() loop
done:        IProvider_release() → dlclose()
```

### Transaction handling

The driver owns transaction state. Each connection has three transaction
handles:

| Handle | Used by | TPB option |
|---|---|---|
| `tra` | statements after `BEGIN`; outside `BEGIN`, statements that change data with `--firebird-autocommit=client` | `--firebird-begin-tpb`, `--firebird-write-tpb` |
| `tra_read` | a SELECT outside `BEGIN`; prepare | `--firebird-read-tpb` |
| `tra_auto` | outside `BEGIN`, statements that change data with `--firebird-autocommit=server` | read committed + `isc_tpb_autocommit` |

`fb_read_transaction()` returns `tra` when it is open (after `BEGIN`, or a
write transaction not committed yet), else `tra_read`, which stays open
between SELECTs. `fb_write_transaction()` returns `tra` when it is open;
outside `BEGIN` it first ends `tra_read`, then starts `tra` (client) or
returns `tra_auto` (server). `fb_start_transaction()` builds the TPB; a
read committed TPB asks for read consistency and falls back once to
`rec_version` on a Firebird 3 server.

**The read transaction is short.** In Firebird 5 an open transaction keeps
`MON$OLDEST_SNAPSHOT` at its start, also a read-only read committed one,
and the garbage collector cannot remove versions newer than that. So
`tra_read` ends before every statement that changes data, after a prepare
that started it, at `BEGIN`, before DDL and at disconnect. A prepared
cursor closes as soon as its rows are read (the close travels with the
next request), so no open cursor stops the commit.

`firebird_drv_query()` intercepts the three control statements sysbench
sends as SQL:

- `BEGIN` → commit any open implicit transaction, `tra_read` and
  `tra_auto`, then start `tra` with `--firebird-begin-tpb`, and set
  `in_explicit_txn`
- `COMMIT` → close any pending `IBatch`, then `fb_commit()`
- `ROLLBACK` → `fb_rollback_quiet()`

**Per-statement autocommit.** When no `BEGIN` was sent, a statement that
changes data commits:

- `--firebird-autocommit=client` (default): `fb_write_transaction()` starts
  `tra`, and the caller commits it right after the statement succeeds.
  3 round trips: `op_transaction`, `op_execute`, `op_commit`.
- `--firebird-autocommit=server`: the statement runs in `tra_auto`, and the
  server commits it (commit retaining). 1 round trip. Commit retaining
  keeps the oldest snapshot of the transaction start, so the driver
  commits `tra_auto` and starts a new one after
  `--firebird-autocommit-restart` statements (default 100).

This matters for benchmark fairness: without it, every insert in a
`prepare` run accumulated in one long transaction and paid no fsync,
which is not what MySQL and PostgreSQL do. A SELECT reads in `tra_read`
and commits nothing, unless `--firebird-autocommit-select` is on.

**Every commit is checked** (`fb_commit()`). A failed commit rolls the
transaction back instead of losing the handle, and the error reaches the
caller.

**DDL outside a transaction** runs in a transaction of its own
(`fb_execute_ddl()`). Firebird does most DDL work at commit time, so DDL on
other connections (a parallel `prepare`) can make the commit fail with a
lock or update conflict; the statement then runs again, up to 10 times.
`fb_execute_ddl()` first ends `tra_read` and `tra_auto`: a transaction
that has used a table keeps it "in use", and `DROP TABLE` on it would
fail.

### Prepared statements

```
prepare:      IAttachment_prepare(..., PREPARE_PREFETCH_METADATA) in the
              read transaction (committed after, when the prepare
              started it)
              → IStatement_getType / getInputMetadata / getOutputMetadata
              → allocate flat in/out byte buffers from getMessageLength()
              → allocate per-column conversion buffers (MAX_COLUMN_LENGTH)
bind_param:   copy sysbench's db_bind_t array; validate parameter count
execute:      fb_set_param() writes each value into in_buf at the metadata
              offset; a SELECT then runs per --firebird-select-mode (below),
              DML through IStatement_execute()
close:        IResultSet_close → release metadata → IStatement_free
```

**One-row execute.** With `--firebird-select-mode=auto` (default) or
`singleton`, a SELECT runs as `IStatement_execute()` with the output message:
the client sends one `op_execute2` instead of `op_free_statement` +
`op_execute` + `op_fetch`, and the SuperServer queues one request instead of
three. The engine runs it as a singleton select:

| Result | Status | Driver action |
|---|---|---|
| one row | success | row is in `out_buf`, extracted in place |
| no row | `isc_stream_eof` | empty result |
| more than one row | `isc_sing_select_err` (-811) | `auto`: set `multi_row`, run again through the cursor, use the cursor from then on; `singleton`: fatal error |

`--firebird-select-mode=cursor` keeps the cursor path for every SELECT.

There is no XSQLDA. Parameters and results live in flat byte buffers, written
and read at the offsets `IMessageMetadata` reports:

```c
unsigned off     = IMessageMetadata_getOffset(meta, st, idx);
unsigned nullOff = IMessageMetadata_getNullOffset(meta, st, idx);
*(ISC_LONG *)(buf + off) = value;
*(short *)(buf + nullOff) = 0;   /* 0 = not null, -1 = null */
```

`BEGIN`/`COMMIT`/`ROLLBACK` and everything else when `--db-ps-mode=disable`
are marked `stmt->emulated`; those substitute values into the SQL text and
route through `fb_query()`.

An emulated statement has no parameters and its SQL text is never reused,
so a server-side prepared statement buys nothing. When the emulated
statement is created, `fb_describe_template()` prepares the template once,
reads its type and frees it. Every execution then sends the literal SQL text
with one attachment-level call:

| Statement | Call |
|---|---|
| SELECT | `IAttachment_openCursor()` with no output metadata; the format comes from `IResultSet_getMetadata()` |
| INSERT, UPDATE, DELETE without RETURNING | `IAttachment_execute()` with no messages |
| anything else | generic path: prepare → openCursor/execute → free |

Over the wire `IAttachment_execute()` is one `op_exec_immediate2` round trip
instead of prepare + execute. Plain `query()` calls have no template to
describe. `fb_text_is_plain_dml()` reads the first word of their text: an
INSERT, UPDATE, DELETE or MERGE without the word RETURNING anywhere also
goes through `IAttachment_execute()` (`oltp_insert` sends its INSERT so:
5 sends per event before, 3 now, 1 with `--firebird-autocommit=server`).
Any other text takes the generic path, which prepares it to learn its
type.

### Result set handling

The OLTP workloads are dominated by single-row point selects. The one-row
execute puts the row into the statement's `out_buf`, and the columns are
extracted in place as in step 1 below. The cursor path works like this:

1. Fetch the first row into the statement's `out_buf` and extract columns
   **in place** with `fb_extract_column_fast()` — integers formatted into
   the statement's per-column conversion buffer, strings pointing directly
   into `out_buf`. No per-row allocation.
2. Fetch again. On `NO_DATA` the result is one row and `rs->ptr` is the
   statement itself; `fetch_row()` returns the cached values.
3. If a second row exists, fall back to the general path: allocate an
   `fb_result_t`, re-extract row 0 from a snapshot taken before the second
   fetch (`row0_buf`, allocated once per statement), then materialise every
   remaining row with `fb_extract_column_alloc()`. The row array doubles
   from a capacity of 64.

`fb_query()` (non-prepared path) always uses the general `fb_result_t` form,
built by `fb_fetch_all()`; a fetch error is reported, not cut short into a
partial result. Either way all rows are client-side before `fetch_row()` is
called, matching how the PostgreSQL driver hands out `PGresult` values.

`fb_free_result()` frees the string allocations tracked in the result's
`strings` array; the single-row fast path has nothing to free.

### Bulk inserts via IBatch

`multi_rows_insert = 0` tells sysbench's bulk layer that Firebird has no
multi-row `VALUES` syntax, so it emits one `INSERT` per row. Instead of
sending them one at a time, `firebird_drv_query()` recognises
`INSERT INTO ... VALUES (...)`, counts the columns, and builds an `IBatch`
over a parameterised form of the same statement:

```
first row:   IAttachment_prepare("INSERT INTO t VALUES(?,?,?,?)")
             → IStatement_createBatch → allocate one message buffer
each row:    parse the literal VALUES list into the message buffer at the
             metadata offsets → IBatch_add()
every 1000:  IBatch_execute() (+ commitRetaining when the transaction is
             the driver's own)
COMMIT:      flush and close the batch before committing
```

A different base statement closes the current batch and opens a new one.
When `createBatch` fails, or the ABI probe disabled batching, the code falls
through to `regular_query` and inserts row by row.

An `INSERT` with more than 120 columns is rejected as a fatal error rather
than falling back — the generated `(?,?,...)` list is built in a fixed 256-byte
buffer. sysbench's OLTP tables have four columns, so this limit has never
been reached in practice.

`db_driver.c` needed two changes for this to work with sysbench's bulk
layer, both additive:

- `bulk_single_row` (set from `!driver_caps.multi_rows_insert`) makes
  `db_bulk_insert_next()` flush after every row instead of accumulating a
  multi-row statement.
- `db_bulk_do_insert()` treated any non-`DB_ERROR_NONE` as failure, which
  aborted on `DB_ERROR_IGNORABLE`. It now checks for `DB_ERROR_FATAL`, so a
  lock conflict during bulk insert is retried rather than fatal.

### Error handling

```c
if (IStatus_getState(st) & IStatus_STATE_ERRORS)
{
  char msg[512];
  IUtil_formatStatus(fb_utl, msg, sizeof(msg), st);
  long sqlcode = p_isc_sqlcode(IStatus_getErrors(st));
}
IStatus_init(st);  /* before the next call */
```

- `fb_handle_error()` fills `con->sql_errno` (SQLCODE), `con->sql_state`
  (zero-padded absolute SQLCODE) and `con->sql_errmsg` from
  `IUtil_formatStatus()`. Both strings are `strdup`ed so they outlive the
  `IStatus`, which the Lua FFI layer requires.
- Lock conflicts (SQLCODE **-913**) and unique violations (**-803**) roll
  the transaction back and return `DB_ERROR_IGNORABLE` — sysbench retries
  the transaction. Everything else is `DB_ERROR_FATAL`.
- SQLCODE **-607** is swallowed by the `CREATE TABLE IF NOT EXISTS` and
  `DROP TABLE IF EXISTS` paths.
- Rollback during error handling uses a fresh `IStatus` so it can't
  overwrite the error being reported.

### SQL dialect handling

All handled inside the driver, never in Lua scripts:

| Feature | How handled |
|---|---|
| No multi-row INSERT | `drv_caps.multi_rows_insert = 0` + `IBatch` |
| No auto-commit | `drv_caps.needs_commit = 1`, driver manages transactions |
| `DROP TABLE IF EXISTS` | Strips `IF EXISTS`, executes, ignores -607 |
| `CREATE TABLE IF NOT EXISTS` | Strips `IF NOT EXISTS`, executes, ignores -607 |
| `GENERATED BY DEFAULT AS IDENTITY` | DDL branch in `oltp_common.lua` |
| Auto-inc insert (omit id column) | Branch in `oltp_insert.lua` |

## Build system integration

The Firebird driver follows the same pattern as MySQL and PostgreSQL:

1. `m4/ac_check_firebird.m4` — honours `--with-firebird=PATH`,
   `--with-firebird-includes` and `--with-firebird-libs`, falling back to
   `fb_config`. It only resolves the **header** path (`FIREBIRD_CFLAGS`);
   `FIREBIRD_LIBS` is `-ldl`, since libfbclient is loaded at runtime.
2. `configure.ac` — `AC_ARG_WITH([firebird])`, `AC_CHECK_FIREBIRD`,
   `AM_CONDITIONAL(USE_FIREBIRD)`, and a line in the configuration summary
3. `src/drivers/firebird/Makefile.am` — builds `libsbfirebird.a`
4. `src/Makefile.am` — links `libsbfirebird.a` + `$(FIREBIRD_LIBS)`
5. `src/db_driver.h` / `src/db_driver.c` — `#ifdef USE_FIREBIRD` registration

The headers must come from Firebird 5 or newer: `fb_c_api.h` first shipped in
FB5, while FB3 and FB4 install only the C++ interface headers. The resulting
binary still runs against an FB4 client library.

## Test infrastructure

11 Cram test files following the MySQL/PostgreSQL test patterns:

- `firebird_common.sh` extracts connection parameters from `SBTEST_FIREBIRD_ARGS`
  and finds `isql` via PATH (no hardcoded paths)
- Tests use `\s* (re)` regex patterns for blank lines (sysbench outputs
  trailing whitespace that Cram is strict about)
- Each test is self-contained: creates tables, runs operations, drops tables
- `script_oltp_general_firebird.t` verifies table structure via `isql`

## CI

The GitHub Actions workflow includes a `build_firebird` job that:

1. Installs build dependencies on Ubuntu
2. Builds Firebird 5.0.4 from source and installs it to `/opt/firebird`
3. Builds sysbench with the Firebird driver
4. Creates the SYSDBA user and starts the Firebird server
5. Runs a full prepare → run → cleanup cycle
