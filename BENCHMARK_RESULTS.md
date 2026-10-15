# Sysbench Benchmark Results — Three-Way (MariaDB vs PostgreSQL vs Firebird 5)

> **Note on these results (added 2026-10-15).** They were measured with
> the driver and scripts of that time. MySQL/MariaDB used the Unix socket
> (no `--mysql-host`), while PostgreSQL and Firebird used TCP; the socket
> alone gives MariaDB about +25 % at 1 thread. Firebird ran every SELECT
> outside BEGIN in one long read-write snapshot transaction, and each
> SELECT used `openCursor` + `fetch`. The current scripts use TCP for all
> DBMS, and the current driver uses a read-only read committed transaction
> and a one-row execute for SELECT (see FIREBIRD.md).

## Environment

- **OS:** Debian 13 (trixie) on WSL2, Linux 6.6.87, x86_64
- **CPU:** AMD (host CPU via WSL2)
- **sysbench:** 1.1.0-844b13c (OO API driver with Batch API + autocommit + dlopen)
- **MariaDB:** 11.8.6, data on `/var/lib/mysql` (disk, ext4)
- **PostgreSQL:** 17.10, data on `/var/lib/postgresql` (disk, ext4)
- **Firebird 5.0.4** (SuperServer), data on `/var/lib/firebird/sbtest.fdb`
- All three with **128 MB buffer cache** matched (Firebird page size 8 KB,
  `DefaultDbCachePages = 16384`).
- All three on the same physical disk with identical mount options.
- **Date:** 2026-06-16

## Methodology

- **Tables:** 4
- **Threads:** 1, 2, 4
- **Duration:** 10 seconds per test
- **Prepared statements:** enabled (`--db-ps-mode=auto`)
- **Methodology:** cleanup + prepare before each test (for 10K suite); 1M
  scaling tests run sequentially on the same prepared dataset.

## TABLE_SIZE=10K (sysbench default), 4 threads, TPS

| Benchmark | MariaDB 11.8 | PostgreSQL 17.10 | Firebird 5.0.4 |
|---|---:|---:|---:|
| oltp_point_select | **60,164** | 44,451 | 14,510 |
| oltp_read_only | **2,591** | 2,293 | 535 |
| oltp_read_write | **891** | 762 | 55 |
| oltp_insert | **1,149** | 1,117 | 153 |
| oltp_delete | **1,368** | 1,311 | 66 |
| oltp_update_index | **1,175** | 1,132 | 55 |
| oltp_update_non_index | **1,168** | 1,140 | 51 |
| select_random_points | 28,330 | **33,051** | 11,021 |
| select_random_ranges | **25,378** | 24,423 | 11,810 |
| bulk_insert | 162,575 | **230,866** | 39,585 |

Measured 2026-10-14 with the driver fix described under *Correction* below.
Each figure is the second of two consecutive runs; see *Run-to-run variance*.

### Caveat: TABLE_SIZE=10K creates an artificial conflict storm

With only 10K rows × 4 tables and 4 concurrent threads, every benchmark
that mutates rows by random ID hits the same row repeatedly. This
produces artificial contention that disproportionately penalises engines
using WAIT-style locking. Numbers should be read alongside the 1M
dataset below.

## TABLE_SIZE=1M, scaling 1 → 2 → 4 threads, TPS

> **Not re-measured.** The figures in this section predate the
> `query()`-insert batching fix and have not been repeated at 1M rows. The
> `oltp_insert` rows are invalid — Firebird's values there were produced by
> the Batch API path and are inflated by roughly an order of magnitude. The
> read rows are unaffected by that fix, but were single runs, and Firebird's
> write rows carry the run-to-run variance described above.


| Benchmark | DB | 1t | 2t | 4t |
|---|---|---:|---:|---:|
| **oltp_point_select** | Firebird | 6,528 | 10,868 | **15,481** |
| | MariaDB | 1,429 | 6,310 | 12,600 |
| | PostgreSQL | 1,808 | 6,125 | 25,515 |
| **oltp_read_only** | Firebird | 166 | 446 | 675 |
| | MariaDB | 155 | 332 | 644 |
| | PostgreSQL | 390 | 798 | **1,877** |
| **oltp_read_write** | Firebird | 52 | 104 | 91 |
| | MariaDB | 85 | 166 | **253** |
| | PostgreSQL | 76 | 134 | 268 |
| **select_random_points** | Firebird | 6,251 | 10,218 | 15,505 |
| | MariaDB | 579 | 1,403 | 2,735 |
| | PostgreSQL | 5,068 | 10,869 | **24,323** |
| **select_random_ranges** | Firebird | 5,998 | 9,831 | 16,185 |
| | MariaDB | 5,339 | 10,300 | **22,361** |
| | PostgreSQL | 1,929 | 3,836 | 7,926 |
| **oltp_update_index** | Firebird | 316 | 524 | 527 |
| | MariaDB | 296 | 482 | **981** |
| | PostgreSQL | 342 | 425 | 904 |
| **oltp_update_non_index** | Firebird | 335 | 373 | 212 |
| | MariaDB | **355** | 395 | 379 |
| | PostgreSQL | 343 | 238 | 343 |
| **oltp_delete** | Firebird | 157 | 205 | 207 |
| | MariaDB | 156 | 191 | 368 |
| | PostgreSQL | 153 | 173 | **380** |

## Analysis

### In-cache reads (10K dataset)

MariaDB and PostgreSQL dominate on small in-cache datasets. MariaDB
hits 56K QPS on point_select, PostgreSQL 43K, Firebird 15K. The
inherent overhead of Firebird's cursor open/close protocol (extra
client-server round trips per query) is real and visible. This isn't
fixable from the driver layer.

### Disk-bound reads (1M dataset)

At 1M, the picture changes. **Firebird is competitive or leads on
point selects and range scans**:

- `oltp_point_select @ 4t`: Firebird 15,481 > MariaDB 12,600,
  PostgreSQL 25,515 wins.
- `select_random_points @ 2t`: Firebird 10,218 ≈ PostgreSQL 10,869.
- `select_random_ranges @ 4t`: Firebird 16,185 — between MariaDB
  (22,361) and PostgreSQL (7,926).

PostgreSQL still wins on scaling beyond 4 threads on most reads.

### Writes

MariaDB/PostgreSQL both scale writes (update/delete) almost linearly
1t → 4t. Firebird scales 1t → 2t but flattens or regresses at 4t —
the engine concurrency limit Dmitry Yemanov flagged for follow-up.

### Insert behaviour

`oltp_insert` at 10K: **Firebird is ~7.5× slower** (153 vs 1,149 for
MariaDB and 1,117 for PostgreSQL).

An earlier revision of this document reported the opposite — Firebird
leading by ~4× at 3,687 TPS, described as a "genuine engine advantage"
and explicitly "not from batching". That was wrong on both counts. See
*Correction* below.

`bulk_insert` at 10K: **PostgreSQL leads at 231K TPS**, then MariaDB at
163K, Firebird at 40K. This benchmark legitimately uses the bulk API, so
batching applies to every engine that has it and the comparison is fair.

## Correction — oltp_insert was measuring the Batch API

`oltp_insert.lua` sends one `INSERT INTO … VALUES (…)` per event through
`con:query()`. The Firebird driver pattern-matched that statement shape and
diverted it into the OO API `IBatch` path, which buffers 1,000 rows per
round trip and commits them with `commitRetaining`. MariaDB and PostgreSQL
committed every statement and paid an fsync each time. The 4× "win" was
that asymmetry, not the engine.

The driver now batches only while a bulk insert is genuinely in progress —
`prepare` and `bulk_insert.lua`, which reach the driver through
`db_bulk_insert_init()`/`done()`. An INSERT issued through `query()` is
executed and committed when the call returns, matching the other drivers.

This is the same class of problem as the tmpfs and buffer-cache issues
listed below: a measurement artifact flattering Firebird. Data loading is
unaffected — `prepare` of 4 × 10k rows still takes 5.0s, and `bulk_insert`
still measures ~40K TPS.

## Run-to-run variance

The earlier "treat ±10% as noise" guidance was too optimistic for
Firebird's write path. Running the identical suite twice back to back:

| | MariaDB / PostgreSQL | Firebird (any version) |
|---|---|---|
| reads | 0.99–1.03× | 0.91–1.27× |
| writes | 0.98–1.00× | **0.37–1.42×** |

MariaDB and PostgreSQL are reproducible to within a few percent. Firebird's
`oltp_delete`, `oltp_update_*` and `oltp_read_only` moved by factors, not
percentages — FB3's `oltp_delete` by 0.37×, FB5's `oltp_read_only` by 1.92×.
Part of that is background garbage collection after a large table is
dropped: FB5's first pass ran right after a 1M-row table was dropped and
its reads were depressed ~2× until a second pass.

Single-run Firebird write figures should not be quoted. Everything in this
document is the second of two consecutive runs.

## What we fixed for fair comparison

1. **Moved Firebird database from tmpfs (`/tmp`) to disk
   (`/var/lib/firebird`).** Per Dmitry's feedback. The earlier
   tmpfs-vs-disk imbalance was the largest source of unfairness.

2. **Matched buffer cache** to ~128 MB across all three engines.

3. **Per-statement autocommit when no explicit BEGIN.** Previously our
   driver opened an implicit transaction and never committed until
   disconnect, batching all inserts into one fsync-free long
   transaction. Now each statement autocommits unless the caller sent
   BEGIN, matching MySQL/PostgreSQL semantics. Only DML autocommits —
   SELECTs leave the transaction open to avoid wasted round-trips.

4. **Same disk, same mount options** for all three database data
   directories.

*Generated on 2026-06-16 by sysbench with Firebird OO API driver*
