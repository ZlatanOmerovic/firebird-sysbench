# Sysbench Benchmark Results — Firebird Cross-Version Comparison

## Environment

- **OS:** Debian 13 (trixie) on WSL2, Linux 6.6.87, x86_64
- **sysbench:** 1.1.0-3ceba0b (OO API driver with Batch API)
- **Firebird 3.0.14** — built from source (clang), SuperServer, port 3053
- **Firebird 4.0.7** — built from source (gcc 14), SuperServer, port 3054
- **Firebird 5.0.4** — built from source (gcc 14), SuperServer, port 3050
- **Date:** 2026-05-28

## Test Parameters

- **Tables:** 4 (except select_random: 1, bulk_insert: per-thread)
- **Table size:** 10,000 rows per table
- **Threads:** 4
- **Duration:** 10 seconds per test
- **Methodology:** cleanup + prepare before each test for reproducibility
- **Note:** FB3 falls back to single-row INSERTs (no Batch API)

## Summary — TPS (transactions per second)

| Benchmark | FB 3.0.14 | FB 4.0.7 | FB 5.0.4 |
|---|---|---|---|
| oltp_point_select | 14,383 | **14,992** | 14,373 |
| oltp_read_only | **670** | 678 | 667 |
| oltp_read_write | 544 | **599** | 556 |
| oltp_insert | 5,660 | **7,715** | 7,035 |
| oltp_delete | 48 | 49 | **72** |
| oltp_update_index | 73 | **101** | 84 |
| oltp_update_non_index | 61 | 59 | **77** |
| select_random_points | 11,365 | **11,666** | 11,323 |
| select_random_ranges | **11,166** | 10,867 | 11,955 |
| bulk_insert | 6,041 | **320,901** | 313,400 |

## Analysis

- **All three versions within ~10%** on most read workloads. Point selects
  are virtually identical (14.4-15.0K TPS).
- **Batch API (FB4+)** delivers 50-53x faster bulk inserts vs FB3's
  single-row fallback. oltp_insert is 25-36% faster on FB4/5 vs FB3.
- **FB4 leads slightly** on read_write and update_index. FB5 leads on
  delete and update_non_index.
- **FB3 is competitive** on reads despite using the older engine — the
  OO API wire protocol is the same across all three versions.

*Generated on 2026-05-28 by sysbench with Firebird OO API driver*
