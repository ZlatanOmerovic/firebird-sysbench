# Firebird Driver for sysbench

## Overview

This fork adds Firebird database support to sysbench, enabling the same
OLTP benchmarks used for MySQL and PostgreSQL to run against Firebird 4, 5,
and 6.

The driver is written in C against the **Firebird OO API** via its C wrapper
(`firebird/fb_c_api.h`). The OO API is the native interface in Firebird 3+:
ISC API calls are translated to it by the yvalve layer, while OO API calls go
directly to the engine. `libfbclient.so` is not linked at build time — it is
loaded with `dlopen` at startup, so one binary can drive several Firebird
versions via `--firebird-client`.

All Firebird-specific code lives in `src/drivers/firebird/`. The OLTP Lua
scripts and core sysbench code remain byte-identical to upstream except for
additive changes (registering the driver in the build system, a DDL branch,
and a bulk-insert fix in `db_driver.c`).

Repository: https://github.com/ZlatanOmerovic/firebird-sysbench
Upstream: https://github.com/akopytov/sysbench

The earlier ISC API (`ibase.h` / XSQLDA) implementation is preserved on the
`firebird-isc` branch. It is the only option for Firebird 3, whose client
library this driver refuses at startup.

## What's delivered

- `src/drivers/firebird/drv_firebird.c` — the driver (~1,900 lines)
- `src/drivers/firebird/Makefile.am` — build integration
- `m4/ac_check_firebird.m4` — autoconf macro locating the Firebird headers
- Build system wiring in `configure.ac`, `src/Makefile.am`,
  `src/drivers/Makefile.am`, `src/db_driver.h`, `src/db_driver.c`
- Firebird DDL branch in `src/lua/oltp_common.lua`, auto-inc branch in
  `src/lua/oltp_insert.lua`
- All 10 bundled OLTP workloads working, with and without prepared statements
- 11 Cram test files, a Firebird CI job, and `run_benchmarks.sh` /
  `run_benchmarks_readonly.sh`
- Published cross-database and cross-version benchmark results (see the
  `BENCHMARK_*.md` files)

Build requires Firebird 5 headers or newer: `fb_c_api.h` first shipped in
FB5. FB3 and FB4 install only the C++ interface headers.

### Command-line options

```
--firebird-db                 Database connection string (e.g. localhost:/tmp/sbtest.fdb)
--firebird-user               User name (default: SYSDBA)
--firebird-password           Password (default: masterkey)
--firebird-client             libfbclient.so to dlopen (default: libfbclient.so)
--firebird-select-mode        auto | singleton | cursor (default: auto)
--firebird-read-tpb           TPB of a SELECT outside BEGIN (default: rc-ro)
--firebird-autocommit-select  Commit after each SELECT outside BEGIN (default: off)
--firebird-begin-tpb          TPB that BEGIN starts (default: snapshot-rw)
--firebird-write-tpb          TPB of a change outside BEGIN (default: snapshot-rw)
--firebird-autocommit         client | server: who commits a change outside BEGIN (default: client)
--firebird-autocommit-restart With server: new transaction after N statements (default: 100; 0 = never)
--firebird-stats              Page and record counters per statement (default: off)
```

TPB values: `snapshot-rw`, `snapshot-ro`, `rc-rw`, `rc-ro`. `rc` is read
committed with read consistency (Firebird 4 and later); with a Firebird 3
server the driver uses `rec_version` and logs a NOTICE once. Every TPB
waits on lock conflicts.

### SELECT execution (`--firebird-select-mode`)

| Mode | A prepared SELECT runs as | Protocol |
|---|---|---|
| `auto` (default) | `IStatement::execute` with an output message; after the first answer "more than one row" (-811), a cursor from then on | `op_execute2`: one operation |
| `singleton` | always `IStatement::execute`; more than one row is a fatal error | `op_execute2` |
| `cursor` | `openCursor` + `fetchNext` (the old behavior) | `op_free_statement` + `op_execute` + `op_fetch` in one packet |

All three are one round trip. The SuperServer puts each operation through
its global request queue, so one operation instead of three gives about
+5 % at 1 thread and +29 % at 8 threads (measured, `oltp_point_select`).
No row is an empty result, not an error. With `--db-ps-mode=disable` a
SELECT has no parameters and is never run again, so it goes through one
`IAttachment::openCursor`; the select mode does not apply.

### Transactions

| Statement | Transaction | End |
|---|---|---|
| SELECT outside BEGIN | the read transaction, `--firebird-read-tpb` (default read-only read committed) | stays open between SELECTs; committed before a statement that changes data, by BEGIN, by DDL and at disconnect; after each SELECT with `--firebird-autocommit-select=on` |
| Prepare | the read transaction | committed after the prepare, when the prepare started it |
| INSERT, UPDATE, DELETE outside BEGIN, `--firebird-autocommit=client` | a write transaction, `--firebird-write-tpb` | committed by the driver after the statement |
| INSERT, UPDATE, DELETE outside BEGIN, `--firebird-autocommit=server` | the autocommit transaction of the connection: read committed with `isc_tpb_autocommit` | the server commits (commit retaining) after each statement; the driver commits it and starts a new one after `--firebird-autocommit-restart` statements, and at BEGIN, DDL and disconnect |
| DDL outside BEGIN | a transaction of its own | committed after the statement; a lock or update conflict (SQLCODE -901, -913) runs it again, up to 10 times |
| Any statement after BEGIN | the explicit transaction, `--firebird-begin-tpb` | COMMIT or ROLLBACK |

Why `rc-ro` for the read transaction: each statement sees the last
committed data, as with autocommit in MySQL and PostgreSQL. The old driver
read in one long read-write snapshot transaction
(`--firebird-read-tpb=snapshot-rw`), which did not see new commits.

Why the read transaction ends before each change: in Firebird 5 an open
transaction keeps the oldest snapshot of the database
(`MON$OLDEST_SNAPSHOT`) at its start, also a read-only read committed
one. Then the garbage collector cannot remove the old record versions
that the updates of the run leave. Update workloads then measure too high
(`oltp_update_non_index` on cc2: +17 % with Forced Writes on, +10 % to
+81 % with Forced Writes off), and up to 190K old versions stay in the
tables after a 30 s run. A connection that only reads keeps its read transaction
open; only writes leave old versions.

### Server autocommit (`--firebird-autocommit=server`)

With `client` (the default), a statement outside BEGIN that changes data
costs 3 round trips: `op_transaction`, `op_execute`, `op_commit`. MariaDB
and PostgreSQL need 1. With `server` the statement runs in a read
committed transaction with `isc_tpb_autocommit`: the server commits after
each statement that changes data, and the client sends only `op_execute`.
The isolation is read committed with read consistency;
`--firebird-write-tpb` does not apply.

Commit retaining keeps the oldest snapshot of the transaction start, as
an open read transaction does. So the driver commits the autocommit
transaction and starts a new one after `--firebird-autocommit-restart`
statements (default 100): 2 more round trips per 100 statements, and the
garbage collector stays at most about 100 x threads transactions behind.
`--firebird-autocommit-restart=0` never ends it; do not publish results
with it, because garbage collection stops for the whole run.

Measured on cc2 (TPS, 4 x 1M rows, golden copy per case,
`results/2026-10-16/writes_b1_b3_cc2.txt`):

| Workload | Forced Writes | Threads | client | server |
|---|---|---|---|---|
| `oltp_update_non_index` | on | 4 / 16 | 655 / 676 | 670 / 645 |
| `oltp_update_non_index` | off, file system cache | 1 / 16 | 7 243 / 15 466 | 14 672 / 21 760 |
| `oltp_insert` | off, file system cache | 1 / 16 | 5 511 / 19 889 | 8 594 / 24 655 |

With Forced Writes ON the disk is the limit, and `server` changes little.
Without synchronous writes, the round trips are a large part of each
statement. `server` with restart 0 gave 38.5K TPS for the 16-thread
update only because garbage collection stopped (189K old versions left).

`--firebird-autocommit-select=on` gives a real autocommit. It is slow in
Firebird, because `op_transaction` and `op_commit` are not deferred: 3
round trips per query. Use it as a separate test, not as the default.

`--firebird-begin-tpb=snapshot-ro` is correct for `oltp_read_only` and for
an application that knows its transaction only reads. A read-write
transaction start writes the header page; with Forced Writes ON this is a
synchronous disk write. MariaDB and PostgreSQL do not need the
declaration, because they do not write at the start of a transaction.
Publish both results for `oltp_read_only`.

### Comparable results

- **Same transport.** Compare DBMS through TCP on the loopback for all of
  them. The MySQL client without `--mysql-host` uses the Unix socket,
  which alone gives MariaDB about +25 % at 1 thread. `run_benchmarks*.sh`
  use TCP by default (`TRANSPORT=tcp`); `TRANSPORT=local` uses the Unix
  sockets and embedded Firebird.
- **LINGER.** With `LINGER` 0 the SuperServer closes the database when the
  last connection ends and drops its page cache, so each sysbench process
  starts cold (7.4K against 15.5K QPS measured at 1M rows). Run
  `ALTER DATABASE SET LINGER TO 60` once. The scripts do it
  (`FIREBIRD_LINGER`).
- **Warm-up.** Use `--warmup-time` (the scripts use `WARMUP`, default
  10 s).
- **Settings in the output.** The first connection of a `run` logs one
  line: server version, page size, page buffers, Forced Writes, the file
  system cache, LINGER and the wire protocol, and warns when a remote
  database has LINGER 0. "file system cache off" means `O_DIRECT`: with
  Forced Writes off each commit still writes its pages to the device, 1.7
  to 2 times slower than with the cache (`UseFileSystemCache = true`, or a
  page cache smaller than `FileSystemCacheThreshold`). It shows "?" when
  the user cannot read `RDB$CONFIG` (administrators only) or the server is
  older than Firebird 4.
- **Garbage collection.** Compare update results only from drivers that
  let the garbage collector work. firebird-sysbench before this change
  kept a read transaction open for the whole run (see Transactions).
- **Autocommit.** State `--firebird-autocommit` with the results.
- **Diagnosis.** `--firebird-stats=on` prints page fetches, page reads,
  page writes, page marks and records read per statement, from the
  database counters (`MON$IO_STATS`, `MON$RECORD_STATS`).

### Driver capabilities

```
multi_rows_insert = 0   (Firebird has no multi-row VALUES syntax)
prepared_statements = 1 (IStatement + IMessageMetadata)
auto_increment = 0      (uses IDENTITY columns instead)
needs_commit = 1        (the driver ends transactions; see --firebird-autocommit)
serial = 0              (not supported)
unsigned_int = 0        (not supported)
```

### Version support

| Version | As server | As client library | Batch API |
|---|---|---|---|
| Firebird 6.0 (trunk) | Tested | Works | Yes |
| Firebird 5.0 | Tested — reference platform | Works | Yes |
| Firebird 4.0 | Tested | Works (cannot be built against) | Yes |
| Firebird 3.0 | Untested | Refused at startup | No |

`drv_init` reads the runtime `IUtil` vtable version. Below v3 is a fatal
error pointing at the `firebird-isc` branch — FB3's client reports v2.
Below v4 the Batch API is switched off, since `IStatement::createBatch`
does not exist that far back; this is a safety net, not a version
statement, as FB4.0.7's client already reports v4 or newer. v4 and up
enables everything, which covers the FB4, FB5 and FB6 clients.

### SQL dialect handling

All SQL dialect differences are handled inside the driver or in a single
Lua DDL branch. The OLTP query templates are unchanged.

| Feature | How it's handled |
|---|---|
| Auto-increment | `GENERATED BY DEFAULT AS IDENTITY` (FB 3+) in Lua DDL |
| Parameters | `?` markers — same as MySQL, no conversion needed |
| Transactions | Driver intercepts `BEGIN`/`COMMIT`/`ROLLBACK` and calls the OO API directly |
| Auto-commit | Outside `BEGIN` each DML and DDL statement commits: the driver commits it, or the server with `--firebird-autocommit=server`; a SELECT reads in the read transaction (see Transactions) |
| SQL text from `con:query()` | An INSERT, UPDATE, DELETE or MERGE without RETURNING runs with one `IAttachment::execute`, without prepare (`oltp_insert`); other text is prepared first to learn its type |
| Multi-row INSERT | `multi_rows_insert=0`; the driver funnels the per-row INSERTs into `IBatch` |
| `DROP TABLE IF EXISTS` | `IF EXISTS` stripped, SQLCODE -607 ignored |
| `CREATE TABLE IF NOT EXISTS` | `IF NOT EXISTS` stripped, SQLCODE -607 ignored |
| Parallel `prepare` | DDL that conflicts at commit with DDL on another connection runs again (issue #6) |

### Stability milestone

64 threads, 10-minute `oltp_read_write` run without errors.

---

## Roadmap

### Done

- **OO API migration.** The driver was rewritten from the ISC API to the OO
  API (was "Phase 7 — optional"). Batch execution, unlimited `IStatus` error
  chains and flat message buffers came with it.
- **Bulk loading.** Superseded the planned `EXECUTE BLOCK` batching: the OO
  API's `IBatch` sends 1,000 rows per round trip, which is both faster and
  simpler than generating `EXECUTE BLOCK` SQL. No `--firebird-bulk-method`
  option was needed — the driver detects `INSERT INTO ... VALUES` in the
  query path and batches automatically, falling back to single-row inserts
  when the runtime client has no Batch API.
- **Cross-version comparisons.** Firebird 4 vs 5 vs 6 measured on identical
  hardware — see `BENCHMARK_FIREBIRD_VERSIONS.md` and
  `BENCHMARK_FIREBIRD_6_vs_5.md`. Firebird 3 was dropped from the matrix
  because of the client-library ABI, not for lack of interest.
- **All bundled OLTP scripts.** All 10 workloads (`oltp_*`,
  `select_random_*`, `bulk_insert`) run against Firebird and appear in the
  benchmark results.
- **Fair cross-database methodology.** Firebird data moved off tmpfs onto
  the same disk as MariaDB/PostgreSQL, buffer caches matched at 128 MB, and
  per-statement autocommit added so transaction (and fsync) semantics match
  the other engines. Results in `BENCHMARK_RESULTS.md` and
  `BENCHMARK_FIREBIRD6_vs_MYSQL_PGSQL.md`.

### Open

**Firebird 6 regressions to investigate.** Two findings from the FB6 trunk
measurements, both worth reducing to a minimal reproducer before reporting:

- The 4M-row `prepare` on the `IBatch` path takes 28 min on FB6 vs ~15 min
  on FB4/FB5 (~87% slower).
- `oltp_read_write` scales 1t → 2t on FB6 but collapses at 4t (44 → 23 TPS
  vs FB5). Reads, by contrast, improved dramatically in FB6.

Since FB6 is a trunk build, some of this may be diagnostic overhead that
won't survive to release.

**Write scaling past 2 threads.** On FB5, `oltp_update_*` and `oltp_delete`
grow from 1t to 2t and then flatten or regress, while MariaDB and PostgreSQL
scale nearly linearly to 4t. Flagged by Dmitry Yemanov for follow-up; needs
engine-side profiling to separate driver cost from engine concurrency limits.

**Firebird architecture comparisons.** Classic vs SuperServer vs
SuperClassic thread-scaling, 1 → 128 threads, on `oltp_read_only` and
`oltp_read_write`. All measurements so far are SuperServer only.

**Upstream merge.** Submit to `akopytov/sysbench`. Prerequisites:

- CI passing with Firebird in the test matrix
- Documentation in upstream README format
- Lua scripts verifiably byte-identical to upstream apart from the DDL branch
- Clean, additive-only git history

**Docker Compose reproducibility kit.** One-command benchmarking with
Firebird, PostgreSQL and MariaDB servers plus a prebuilt sysbench runner and
a results collector.

**Windows native support (optional).** Firebird runs natively on Windows;
whether the driver builds with MinGW or MSVC is untested. Windows users
currently need WSL2.

---

## Design Principles

1. **All Firebird code in one directory** — `src/drivers/firebird/`
2. **Upstream Lua scripts byte-identical** — except the DDL branch in
   `oltp_common.lua` and the auto-inc branch in `oltp_insert.lua`,
   verifiable with `git diff upstream/master -- src/lua/`
3. **SQL dialect differences inside the driver** — never in workload scripts
4. **Only additive upstream edits** — registering the driver in build files
5. **OO API, loaded at runtime** — direct engine access, and one binary that
   can benchmark several Firebird versions
