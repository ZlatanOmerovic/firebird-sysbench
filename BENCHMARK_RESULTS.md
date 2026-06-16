# Sysbench Benchmark Results — Three-Way Comparison

## Environment

- **OS:** Debian 13 (trixie) on WSL2, Linux 6.6.87, x86_64
- **CPU:** AMD (host CPU via WSL2)
- **sysbench:** 1.1.0-3ceba0b (OO API driver with Batch API)
- **MariaDB:** 11.8.6, data on `/var/lib/mysql` (disk, ext4)
- **PostgreSQL:** 17.10, data on `/var/lib/postgresql` (disk, ext4)
- **Firebird:** 5.0.4 (SuperServer), data on `/var/lib/firebird` (disk, ext4)
- **Date:** 2026-06-16

## Methodology

All databases run on the same physical disk with identical mount options
and matched buffer cache sizes (~128MB). Firebird uses per-statement
autocommit when no explicit `BEGIN` is sent, matching MySQL/PostgreSQL.
This makes the per-row fsync cost comparable across all three engines.

- **Tables:** 4
- **Threads:** 1, 2, 4
- **Duration:** 10 seconds per test
- **Prepared statements:** enabled (`--db-ps-mode=auto`)
- **Methodology:** cleanup + prepare before each test

Two table sizes reported:
- **TABLE_SIZE=10K** (sysbench default) — small dataset where conflicts
  dominate writes
- **TABLE_SIZE=1M** — large dataset where cache misses and engine
  concurrency dominate

## TABLE_SIZE=10K (4 threads)

| Benchmark | MariaDB 11.8 | PostgreSQL 17.10 | Firebird 5.0.4 |
|---|---|---|---|
| oltp_point_select | **54,225** | 33,979 | 14,951 |
| oltp_read_only | **2,472** | 2,046 | 570 |
| oltp_read_write | **818** | 637 | 52 |
| oltp_insert | 1,014 | 951 | **4,010** |
| oltp_delete | **1,164** | 1,113 | 280 |
| oltp_update_index | **1,056** | 983 | 132 |
| oltp_update_non_index | **1,003** | 998 | 168 |
| select_random_points | 24,487 | **30,411** | 12,114 |
| select_random_ranges | **24,225** | 23,397 | 13,090 |
| bulk_insert | 161,416 | **340,428** | 39,710 |

### Caveat: TABLE_SIZE=10K creates an artificial conflict storm

With only 10,000 rows × 4 tables and 4 concurrent threads, every benchmark
that mutates rows by random ID hits the same row repeatedly. This produces
artificial contention that disproportionately penalizes engines using
WAIT-style locking. Numbers should be read alongside the 1M dataset below.

## TABLE_SIZE=1M, scaling 1 → 2 → 4 threads

| Benchmark | DB | 1t | 2t | 4t |
|---|---|---|---|---|
| **oltp_point_select** | Firebird | 2,276 | 5,266 | 9,428 |
| | MariaDB | 3,114 | 5,952 | 9,968 |
| | PostgreSQL | 5,293 | 14,222 | **33,072** |
| **oltp_read_only** | Firebird | 69 | 143 | 195 |
| | MariaDB | 143 | 328 | 636 |
| | PostgreSQL | 385 | 769 | **1,617** |
| **oltp_read_write** | Firebird | 20 | 29 | 44 |
| | MariaDB | 80 | 155 | 200 |
| | PostgreSQL | 82 | 152 | **206** |
| **select_random_points** | Firebird | 645 | 1,396 | 2,837 |
| | MariaDB | 508 | 1,224 | 2,582 |
| | PostgreSQL | 4,857 | 10,073 | **21,433** |
| **select_random_ranges** | Firebird | 143 | 307 | 613 |
| | MariaDB | 4,348 | 9,852 | **21,435** |
| | PostgreSQL | 1,790 | 3,652 | 6,585 |
| **oltp_update_index** | Firebird | 63 | 47 | 45 |
| | MariaDB | 376 | 496 | **935** |
| | PostgreSQL | 141 | 166 | 386 |
| **oltp_update_non_index** | Firebird | 36 | 50 | 66 |
| | MariaDB | 345 | 190 | **379** |
| | PostgreSQL | 176 | 193 | 374 |
| **oltp_delete** | Firebird | 45 | 61 | 81 |
| | MariaDB | 168 | 214 | 379 |
| | PostgreSQL | 175 | 195 | **389** |

## Analysis

- **Point selects:** Firebird matches MariaDB at 4 threads (9.4K vs 10K).
  PostgreSQL pulls away to 33K.

- **select_random_points:** Firebird actually beats MariaDB at 1-2
  threads (645/1.4K vs 508/1.2K) and ties at 4t. PostgreSQL is ~8×
  faster here.

- **select_random_ranges:** The biggest single gap. MariaDB hits
  21,435 TPS, PostgreSQL 6,585, Firebird 613 — a 35× and 11× deficit
  respectively. Range-scan plan/execution efficiency on Firebird is
  worth investigating.

- **Write workloads** (update, delete, read_write): Firebird is
  5-20× behind. The most striking pattern: MariaDB and PostgreSQL scale
  roughly 2× from 1 to 4 threads, while Firebird stays flat or
  regresses. This is the engine concurrency limitation Dmitry Yemanov is
  investigating on the Firebird side.

- **oltp_insert:** Firebird wins by ~4× at TABLE_SIZE=10K. Genuine
  engine advantage on small-dataset INSERTs once fairness is restored
  (autocommit per row, no Batch API in `oltp_insert`).

- **bulk_insert:** PostgreSQL wins fairly (340K vs 161K vs 40K) when
  Firebird is on disk. The earlier headline 313K Firebird number was
  tmpfs.

## What we fixed for fair comparison

1. **Moved Firebird database from tmpfs (`/tmp`) to disk
   (`/var/lib/firebird`).** Required after Dmitry Yemanov pointed out
   the storage discrepancy — `/tmp` was tmpfs on this WSL2 host,
   giving Firebird a RAM advantage MariaDB/PostgreSQL didn't have.

2. **Matched buffer cache to 128MB** (Firebird
   `DefaultDBCachePages = 16384`, page size 8KB). MySQL/PostgreSQL
   defaults are also ~128MB.

3. **Per-statement autocommit when no explicit BEGIN.** Previously our
   driver opened an implicit transaction and never committed it until
   disconnect. This batched INSERTs unfairly into a single fsync-free
   long transaction. Now each statement autocommits unless the caller
   sent BEGIN, matching MySQL/PostgreSQL semantics. Only DML
   autocommits — SELECTs leave the transaction open to avoid wasted
   commit round-trips.

*Generated on 2026-06-16 by sysbench with Firebird OO API driver*
