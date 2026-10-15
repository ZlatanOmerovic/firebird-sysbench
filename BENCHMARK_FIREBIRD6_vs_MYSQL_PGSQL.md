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
| oltp_point_select | **60,164** | 44,451 | 15,331 |
| oltp_read_only | **2,591** | 2,293 | 563 |
| oltp_read_write | **891** | 762 | 59 |
| oltp_insert | **1,149** | 1,117 | 149 |
| oltp_delete | **1,368** | 1,311 | 94 |
| oltp_update_index | **1,175** | 1,132 | 48 |
| oltp_update_non_index | **1,168** | 1,140 | 62 |
| select_random_points | 28,330 | **33,051** | 11,752 |
| select_random_ranges | **25,378** | 24,423 | 10,984 |
| bulk_insert | 162,575 | **230,866** | 45,086 |

Measured 2026-10-14, second of two consecutive runs. See the *Correction*
and *Run-to-run variance* sections of `BENCHMARK_RESULTS.md`.

The `oltp_insert` figure previously reported here (5,047 TPS, described as
Firebird's OO API path being "genuinely fast") was an artifact of the driver
batching `query()` inserts. With that fixed, FB6 measures 149 TPS against
MariaDB's 1,149 — roughly 7.7× slower, in line with FB3/FB4/FB5. See the
*Correction* section of `BENCHMARK_RESULTS.md`.

## TABLE_SIZE=1M, scaling 1 → 2 → 4 threads, TPS

> **Not re-measured.** The figures in this section predate the
> `query()`-insert batching fix and have not been repeated at 1M rows. The
> `oltp_insert` rows are invalid — Firebird's values there were produced by
> the Batch API path and are inflated by roughly an order of magnitude. The
> read rows are unaffected by that fix, but were single runs, and Firebird's
> write rows carry the run-to-run variance described above.


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

- **`select_random_points` @ 1-2 threads (1M)**: 2,581 / 5,184 — better
  than MariaDB (579 / 1,403), competitive with PostgreSQL (5,068 /
  10,869).

### Where FB6 loses

- **`oltp_insert` @ 10K**: 149 TPS against MariaDB's 1,149 and
  PostgreSQL's 1,117 — roughly **7.7× slower**. This row previously
  claimed the opposite; see the correction note above the 10K table.
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

- `oltp_insert` is now within noise of FB5 (153 vs 149). The earlier
  claim that FB6 "keeps and improves the advantage" (3,687 → 5,047)
  described the Batch API path, not the engine.
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
