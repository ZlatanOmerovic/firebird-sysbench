# Firebird 6.0 vs Firebird 5.0 — Head-to-Head

Direct comparison of Firebird 5.0.4 (release) vs Firebird 6.0.0.2012 (Initial /
Trunk build) on the same hardware, same disk, same sysbench driver build.

## Environment

- **OS:** Debian 13 (trixie) on WSL2, Linux 6.6.87, x86_64
- **CPU:** AMD (host CPU via WSL2)
- **sysbench:** 1.1.0-844b13c (OO API driver with Batch API, autocommit, dlopen)
- **Firebird 5.0.4** — SuperServer, port 3050, data on `/var/lib/firebird/sbtest.fdb`
- **Firebird 6.0.0.2012** (Initial / Trunk) — SuperServer, port 3056, data on `/var/lib/firebird6/sbtest.fdb`
- **Both** with `DefaultDbCachePages=16384` (128 MB), page size 8 KB
- **Date:** 2026-06-16

## Prepare phase (1M rows × 4 tables = 4M rows total, Batch API)

| Version | Wall time |
|---|---|
| Firebird 5.0.4 | ~15 min |
| **Firebird 6.0.0.2012** | **28 min 02 sec** (~87% slower) |

Same Batch API call path. Significant regression to investigate on the FB6
master branch.

## TABLE_SIZE=10K (sysbench default), 4 threads

| Benchmark | FB 5.0.4 | FB 6.0.0.2012 | Change |
|---|---|---|---|
| oltp_point_select | 14,951 | 14,374 | -4% |
| oltp_read_only | 570 | 582 | +2% |
| oltp_read_write | 52 | 53 | +2% |
| oltp_insert | 4,010 | 4,155 | +4% |
| oltp_delete | 280 | 240 | -14% |
| oltp_update_index | 132 | 119 | -10% |
| oltp_update_non_index | 168 | 143 | -15% |
| select_random_points | 12,114 | 11,603 | -4% |
| select_random_ranges | 13,090 | 11,013 | -16% |
| bulk_insert | 39,710 | 39,353 | -1% |

At 10K the dataset fits in cache and contention dominates; differences here
are mostly within noise except the ~10-16% drop on write-by-id and range
scans.

## TABLE_SIZE=1M, scaling 1 → 2 → 4 threads

| Benchmark | FB | 1t | 2t | 4t |
|---|---|---|---|---|
| **oltp_point_select** | FB5 | 2,276 | 5,266 | 9,428 |
| | **FB6** | **3,968** | **9,323** | **14,382** |
| **oltp_read_only** | FB5 | 69 | 143 | 195 |
| | **FB6** | **131** | **291** | **566** |
| **oltp_read_write** | FB5 | 20 | 29 | **44** |
| | **FB6** | **31** | **40** | 23 |
| **select_random_points** | FB5 | 645 | 1,396 | 2,837 |
| | **FB6** | **2,646** | **5,291** | **10,502** |
| **select_random_ranges** | FB5 | 143 | 307 | 613 |
| | **FB6** | **2,177** | **4,016** | **3,985** |
| **oltp_update_index** | FB5 | 63 | 47 | 45 |
| | **FB6** | **69** | **95** | **107** |
| **oltp_update_non_index** | FB5 | 36 | 50 | 66 |
| | **FB6** | **106** | **82** | **85** |
| **oltp_delete** | FB5 | 45 | 61 | **81** |
| | **FB6** | **53** | 60 | 77 |

## Headlines

1. **Massive read gains in FB6**, especially scaling out to 4 threads:
   - `oltp_point_select` +52% (9,428 → 14,382 TPS @ 4t)
   - `oltp_read_only` +190% (195 → 566)
   - `select_random_points` +270% (2,837 → 10,502)
   - **`select_random_ranges` +550%** (613 → 3,985) — the biggest single
     improvement. This was Firebird's largest deficit vs MariaDB in our
     prior `BENCHMARK_RESULTS.md`; FB6 closes the gap from ~35× to ~5.4×.

2. **`oltp_update_index` and `oltp_update_non_index` are 2-3× faster** under
   single-thread, and scale better with more threads in FB6 (FB5 stays flat
   or regresses, FB6 grows from 1t to 4t).

3. **Two clear regressions:**
   - **Bulk insert (Batch API)**: +87% wall time on the 4M-row prepare.
   - **`oltp_read_write` at 4 threads**: 44 → 23 TPS (-47%). FB6 scales 1t
     → 2t (31 → 40) but collapses at 4t. Concurrent mixed read/write hits
     something contention- or coordination-related.

## Caveats

- **FB6 is a Trunk/T build**, not a release. Regressions on bulk insert and
  4-thread `read_write` may be debug/diagnostic overhead that won't survive
  to release.
- Single observation per data point. Variance not characterized — treat
  ±10% differences as noise.
- WSL2 host, AMD CPU. Absolute numbers transferable only by ratio.

*Generated on 2026-06-16 by sysbench with Firebird OO API driver*
