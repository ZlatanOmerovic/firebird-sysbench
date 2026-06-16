# Sysbench Benchmark Results — Three-Way Comparison

## Environment

- **OS:** Debian 13 (trixie) on WSL2, Linux 6.6.87, x86_64
- **CPU:** AMD (host CPU via WSL2)
- **sysbench:** 1.1.0-3ceba0b (OO API driver with Batch API)
- **MariaDB:** 11.8.6 (Debian package), data on `/var/lib/mysql` (disk, ext4)
- **PostgreSQL:** 17.10 (Debian package), data on `/var/lib/postgresql` (disk, ext4)
- **Firebird:** 5.0.4 (built from source, SuperServer), data on `/var/lib/firebird` (disk, ext4)
- **Date:** 2026-06-16

## Methodology

All databases are on the same physical disk with identical mount options. The
Firebird database was originally on `/tmp` (tmpfs/RAM), which produced
artificially inflated numbers. Per feedback from Dmitry Yemanov (Firebird Lead
Developer), the database was moved to `/var/lib/firebird` for a fair
comparison. **All numbers below reflect honest on-disk results.**

- **Tables:** 4
- **Table size:** 10,000 rows per table (sysbench default)
- **Threads:** 4
- **Duration:** 10 seconds per test
- **Prepared statements:** enabled (server-side, `--db-ps-mode=auto`)

## Summary — TPS at TABLE_SIZE=10,000 (sysbench default)

| Benchmark | MariaDB 11.8 | PostgreSQL 17.10 | Firebird 5.0.4 |
|---|---|---|---|
| oltp_point_select | **54,225** | 33,979 | 14,279 |
| oltp_read_only | **2,472** | 2,046 | 549 |
| oltp_read_write | **818** | 637 | 51 |
| oltp_insert | 1,014 | 951 | **3,782** |
| oltp_delete | **1,164** | 1,113 | 45 |
| oltp_update_index | **1,056** | 983 | 65 |
| oltp_update_non_index | **1,003** | 998 | 47 |
| select_random_points | 24,487 | **30,411** | 11,449 |
| select_random_ranges | **24,225** | 23,397 | 12,324 |
| bulk_insert | 161,416 | **340,428** | 37,357 |

## TABLE_SIZE=10K caveat (sysbench default is misleading)

With only 10,000 rows × 4 tables and 4 concurrent threads, every benchmark
that mutates rows by random ID hits the same row repeatedly. This produces
an artificial **conflict storm** that disproportionately penalizes engines
using WAIT-style locking. We re-ran the affected benchmarks at
TABLE_SIZE=1,000,000 to see the engines without the conflict bias:

### TPS at TABLE_SIZE=1M, varying thread count

| Benchmark | DB | 1 thread | 2 threads | 4 threads |
|---|---|---|---|---|
| **oltp_update_index** | Firebird | 176 | 193 | 186 |
| | MariaDB | 117 | 144 | 226 |
| | PostgreSQL | 168 | 183 | 374 |
| **oltp_update_non_index** | Firebird | 220 | 239 | 183 |
| | MariaDB | 138 | 170 | 374 |
| | PostgreSQL | 164 | 177 | 378 |
| **oltp_delete** | Firebird | 192 | 223 | 144 |
| | MariaDB | 169 | 208 | 375 |
| | PostgreSQL | 174 | 201 | 400 |

Across all 27 runs above, **`ignored_errors=0`** for every database —
WAIT-style locking blocks rather than reports conflicts as errors.

## Analysis

- **At TABLE_SIZE=10K**, Firebird looks 18-25× slower than MariaDB on
  update/delete workloads (e.g. 45 vs 1,164 TPS on oltp_delete). This is the
  **conflict storm artifact**, not engine performance.

- **At TABLE_SIZE=1M with 1 thread**, Firebird is *competitive or faster*
  than MariaDB across update/delete — the engine itself is fast.

- **Firebird does not scale beyond ~2 threads** in this workload (flat or
  slight regression from 2t to 4t). MariaDB and PostgreSQL scale ~2×
  from 1t to 4t. This is the actual concurrency story to investigate, and
  Dmitry Yemanov is looking into it on the Firebird side.

- **Pure reads (point_select, read_only):** MariaDB/InnoDB buffer pool
  wins decisively. Firebird's cursor open/close protocol overhead is real
  and not addressable from the driver layer.

- **bulk_insert was a tmpfs illusion:** earlier 313K TPS for Firebird
  collapsed to 37K when the database moved from tmpfs to disk. PostgreSQL
  wins this benchmark fairly (340K).

- **oltp_insert: Firebird leads (3.8K vs ~1K)** thanks to the OO API
  Batch API — and this advantage holds on disk.

*Generated on 2026-06-16 by sysbench with Firebird OO API driver*
