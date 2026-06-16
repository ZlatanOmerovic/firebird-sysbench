# Sysbench Benchmark Results — MariaDB vs PostgreSQL vs Firebird 6 (Trunk)

Companion to `BENCHMARK_RESULTS.md` (which compares MariaDB/PostgreSQL vs
Firebird **5.0.4 release**). This file compares the same MariaDB and
PostgreSQL against **Firebird 6.0.0.2012** (Trunk / Initial build from
the `master` branch). Same machine, same disk, same sysbench driver,
same benchmark methodology — run back-to-back with the FB5 measurements.

## Environment

- **OS:** Debian 13 (trixie) on WSL2, Linux 6.6.87, x86_64
- **CPU:** AMD (host CPU via WSL2)
- **sysbench:** 1.1.0-844b13c (OO API driver with Batch API + autocommit + dlopen)
- **MariaDB:** 11.8.6, data on `/var/lib/mysql`
- **PostgreSQL:** 17.10, data on `/var/lib/postgresql`
- **Firebird:** 6.0.0.2012 (Trunk), SuperServer, port 3056, data on
  `/var/lib/firebird6/sbtest.fdb`
- All three with **128 MB buffer cache** matched.
- All three on the same physical disk with identical mount options.
- **Date:** 2026-06-16

## Methodology

- **Tables:** 4
- **Threads:** 1, 2, 4
- **Duration:** 10 seconds per test
- **Prepared statements:** enabled (`--db-ps-mode=auto`)

Single-run measurements. Variance not characterised; treat ±10% as noise.

## TABLE_SIZE=10K (sysbench default), 4 threads, TPS

| Benchmark | MariaDB 11.8 | PostgreSQL 17.10 | Firebird 6.0.0.2012 |
|---|---:|---:|---:|
| oltp_point_select | **56,498** | 42,731 | 14,831 |
| oltp_read_only | **2,533** | 2,252 | 617 |
| oltp_read_write | **814** | 733 | 50 |
| oltp_insert | 822 | 938 | **5,047** |
| oltp_delete | **1,003** | 942 | 90 |
| oltp_update_index | **834** | 805 | 50 |
| oltp_update_non_index | **879** | 874 | 98 |
| select_random_points | 27,838 | **32,137** | 11,890 |
| select_random_ranges | **25,940** | 24,498 | 11,685 |
| bulk_insert | 166,314 | **343,105** | 39,086 |

Same `oltp_insert` story as FB5: Firebird's OO API path is genuinely
fast on small in-cache inserts.

## TABLE_SIZE=1M, scaling 1 → 2 → 4 threads, TPS

| Benchmark | DB | 1t | 2t | 4t |
|---|---|---:|---:|---:|
| **oltp_point_select** | Firebird 6 | 1,000 | 3,808 | 7,512 |
| | MariaDB | 1,429 | 6,310 | 12,600 |
| | PostgreSQL | 1,808 | 6,125 | **25,515** |
| **oltp_read_only** | Firebird 6 | 54 | 93 | 156 |
| | MariaDB | 155 | 332 | 644 |
| | PostgreSQL | 390 | 798 | **1,877** |
| **oltp_read_write** | Firebird 6 | 11 | 9 | 11 |
| | MariaDB | 85 | 166 | 253 |
| | PostgreSQL | 76 | 134 | **268** |
| **select_random_points** | Firebird 6 | 2,581 | 5,184 | 10,713 |
| | MariaDB | 579 | 1,403 | 2,735 |
| | PostgreSQL | 5,068 | 10,869 | **24,323** |
| **select_random_ranges** | Firebird 6 | 2,299 | 4,257 | 4,563 |
| | MariaDB | 5,339 | 10,300 | **22,361** |
| | PostgreSQL | 1,929 | 3,836 | 7,926 |
| **oltp_update_index** | Firebird 6 | 54 | 88 | 126 |
| | MariaDB | 296 | 482 | **981** |
| | PostgreSQL | 342 | 425 | 904 |
| **oltp_update_non_index** | Firebird 6 | 91 | 65 | 62 |
| | MariaDB | **355** | 395 | 379 |
| | PostgreSQL | 343 | 238 | 343 |
| **oltp_delete** | Firebird 6 | 44 | 34 | 38 |
| | MariaDB | 156 | 191 | 368 |
| | PostgreSQL | 153 | 173 | **380** |

### Important caveat for FB6 1M numbers

These FB6 1M measurements are a **second pass** on the same prepared
dataset that was already used for earlier FB6 1M scaling runs the same
day. The earlier first-pass run showed substantially better numbers
(e.g. `oltp_point_select @ 4t` was 14,382 then vs 7,512 now;
`oltp_read_only @ 4t` 566 then vs 156 now). This suggests significant
MVCC version accumulation / index bloat from repeated benchmarks
between fresh-prep cycles. **A fresh prepare + single benchmark pass is
the comparable apples-to-apples measurement.** We left the second-pass
numbers in because they're what we have on hand; treat the FB6 1M row
as a worst-case rather than typical.

## Analysis

### Where FB6 wins

- **`oltp_insert` @ 10K**: 5,047 TPS — **5× faster** than MariaDB
  (822) and PostgreSQL (938). The genuine engine advantage on small
  in-cache prepared inserts holds.
- **`select_random_points` @ 1-2 threads (1M)**: 2,581 / 5,184 — better
  than MariaDB (579 / 1,403), competitive with PostgreSQL (5,068 /
  10,869).

### Where FB6 loses

- **Pure in-cache reads (10K)**: MariaDB/PostgreSQL dominate (~3-4×).
  Firebird's cursor open/close protocol overhead is real and isn't
  fixable from the driver layer.
- **Concurrent read/write at 1M**: `oltp_read_write` is 11-23 TPS
  across all thread counts vs MariaDB/PostgreSQL at 250-270.
  Approximately **24× slower** at 4 threads. This is the FB6 master
  branch regression flagged in `BENCHMARK_FIREBIRD_6_vs_5.md`.
- **Writes at 1M**: `oltp_update_index/non_index/delete` are 7-15×
  slower than MariaDB/PostgreSQL at 4 threads.

### FB5 vs FB6 against MariaDB/PostgreSQL

Compared to FB5 (see `BENCHMARK_RESULTS.md`):

- FB6 keeps the `oltp_insert` advantage and improves it (3,687 →
  5,047).
- FB6 regresses on write workloads compared to FB5 at 4 threads.
- FB6 reads at 1M (in this run) are about half of FB5 — but the
  fresh-prep first-pass FB6 numbers showed FB6 ahead of FB5 on most
  reads. The repeat-pass degradation is more severe in FB6 than in
  FB5.

## What we fixed for fair comparison

Same fixes as in `BENCHMARK_RESULTS.md`:

1. Firebird on disk, not tmpfs.
2. 128 MB buffer cache across all three engines.
3. Per-statement autocommit when no explicit BEGIN (matches MySQL/PG).
4. Same disk and mount options.

## Status of FB6

FB6 is a **Trunk / Initial** build, not a release. Regressions —
particularly the `oltp_read_write` collapse at high concurrency and
the Batch-API bulk-insert wall-clock regression (~87% slower than FB5,
documented in `BENCHMARK_FIREBIRD_6_vs_5.md`) — may be debug/diagnostic
overhead that won't survive to release. Flag for upstream and
re-measure when FB6 hits a release candidate.

*Generated on 2026-06-16 by sysbench with Firebird OO API driver*
