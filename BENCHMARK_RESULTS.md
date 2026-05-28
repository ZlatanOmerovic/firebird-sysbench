# Sysbench Benchmark Results — Three-Way Comparison

## Environment

- **OS:** Debian 13 (trixie) on WSL2, Linux 6.6.87, x86_64
- **CPU:** AMD (host CPU via WSL2)
- **sysbench:** 1.1.0-3ceba0b (OO API driver with Batch API)
- **MariaDB:** 11.8.6 (Debian package)
- **PostgreSQL:** 17.10 (Debian package)
- **Firebird:** 5.0.4 (built from source, SuperServer)
- **Date:** 2026-05-28

## Test Parameters

- **Tables:** 4 (except select_random: 1, bulk_insert: per-thread)
- **Table size:** 10,000 rows per table
- **Threads:** 4
- **Duration:** 10 seconds per test
- **Prepared statements:** enabled (server-side, `--db-ps-mode=auto`)
- **Methodology:** cleanup + prepare before each test for reproducibility

## Summary — TPS (transactions per second)

| Benchmark | MariaDB 11.8 | PostgreSQL 17.10 | Firebird 5.0.4 |
|---|---|---|---|
| oltp_point_select | **56,268** | 41,871 | 14,373 |
| oltp_read_only | **2,651** | 2,264 | 667 |
| oltp_read_write | **820** | 755 | 556 |
| oltp_insert | 1,064 | 1,032 | **7,035** |
| oltp_delete | 1,214 | **1,245** | 72 |
| oltp_update_index | **1,051** | 1,047 | 84 |
| oltp_update_non_index | **1,034** | 627 | 77 |
| select_random_points | 26,519 | **32,714** | 11,323 |
| select_random_ranges | **24,597** | 24,555 | 11,955 |
| bulk_insert | 182,900 | 201,585 | **313,400** |

## Analysis

- **Pure reads:** MariaDB leads with InnoDB buffer pool. PostgreSQL second.
  Firebird's cursor open/close overhead is the main factor (~2 IPC per query
  vs MySQL's 1).
- **oltp_insert:** Firebird **7x faster** than MySQL/PostgreSQL thanks to the
  OO API Batch API — rows are batched client-side and flushed every 1000 rows.
- **bulk_insert:** Firebird **313K TPS** via Batch API, beating both MySQL (183K)
  and PostgreSQL (202K).
- **oltp_read_write:** All three databases within 1.5x of each other on the
  mixed workload that most resembles real applications.
- **WSL2 note:** Results are relative, not absolute. Ratios between databases
  matter more than raw numbers.

*Generated on 2026-05-28 by sysbench with Firebird OO API driver*
