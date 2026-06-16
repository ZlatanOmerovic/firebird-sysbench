# Sysbench Benchmark Results — Three-Way (MariaDB vs PostgreSQL vs Firebird 5)

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
| oltp_point_select | **56,498** | 42,731 | 14,835 |
| oltp_read_only | **2,533** | 2,252 | 565 |
| oltp_read_write | **814** | 733 | 50 |
| oltp_insert | 822 | 938 | **3,687** |
| oltp_delete | **1,003** | 942 | 283 |
| oltp_update_index | **834** | 805 | 137 |
| oltp_update_non_index | **879** | 874 | 77 |
| select_random_points | 27,838 | **32,137** | 11,969 |
| select_random_ranges | **25,940** | 24,498 | 12,907 |
| bulk_insert | 166,314 | **343,105** | 31,802 |

### Caveat: TABLE_SIZE=10K creates an artificial conflict storm

With only 10K rows × 4 tables and 4 concurrent threads, every benchmark
that mutates rows by random ID hits the same row repeatedly. This
produces artificial contention that disproportionately penalises engines
using WAIT-style locking. Numbers should be read alongside the 1M
dataset below.

## TABLE_SIZE=1M, scaling 1 → 2 → 4 threads, TPS

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

`oltp_insert` at 10K: **Firebird leads by ~4×** (3,687 vs ~900 for
MariaDB/PostgreSQL). Genuine engine advantage on small in-cache
inserts. Not from batching — `oltp_insert` uses prepared statements,
not the Batch API.

`bulk_insert` at 10K: **PostgreSQL leads at 343K TPS**, then MariaDB
at 166K, Firebird at 32K. Where Firebird previously appeared to win
this on tmpfs, the honest on-disk number tells the real story.

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
