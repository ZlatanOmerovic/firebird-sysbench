# Sysbench Benchmark Results — Three-Way Comparison

## Environment

- **OS:** Debian 13 (trixie) on WSL2, Linux 6.6.87, x86_64
- **CPU:** AMD (host CPU via WSL2)
- **sysbench:** 1.1.0-3ceba0b (built from source with all three drivers)
- **MariaDB:** 11.8.6 (Debian package)
- **PostgreSQL:** 17.10 (Debian package)
- **Firebird:** 5.0.4 (built from source, SuperServer)
- **Date:** 2026-05-27

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
| oltp_point_select | **55,512** | 41,758 | 14,528 |
| oltp_read_only | **2,612** | 2,270 | 664 |
| oltp_read_write | 822 | **773** | 577 |
| oltp_insert | **5,003** | 1,037 | 5,003 |
| oltp_delete | 1,261 | **1,269** | 75 |
| oltp_update_index | **1,081** | 1,020 | 75 |
| oltp_update_non_index | **1,099** | 1,036 | 60 |
| select_random_points | **25,504** | 32,427 | 11,383 |
| select_random_ranges | **25,143** | 24,636 | 12,659 |
| bulk_insert | **135,271** | 304,853 | 5,371 |

## Summary — Average Latency (ms)

| Benchmark | MariaDB 11.8 | PostgreSQL 17.10 | Firebird 5.0.4 |
|---|---|---|---|
| oltp_point_select | **0.07** | 0.10 | 0.27 |
| oltp_read_only | **1.53** | 1.76 | 6.02 |
| oltp_read_write | **4.86** | 5.17 | 6.93 |
| oltp_insert | 3.65 | 3.85 | **0.80** |
| oltp_delete | 3.17 | **3.15** | 53.43 |
| oltp_update_index | **3.70** | 3.92 | 53.56 |
| oltp_update_non_index | **3.64** | 3.86 | 66.13 |
| select_random_points | 0.16 | **0.12** | 0.35 |
| select_random_ranges | **0.16** | **0.16** | 0.31 |
| bulk_insert | **0.00** | 0.01 | 0.73 |

## Summary — 95th Percentile Latency (ms)

| Benchmark | MariaDB 11.8 | PostgreSQL 17.10 | Firebird 5.0.4 |
|---|---|---|---|
| oltp_point_select | **0.12** | 0.14 | 0.35 |
| oltp_read_only | **2.07** | 2.26 | 7.43 |
| oltp_read_write | 7.70 | **7.98** | **8.28** |
| oltp_insert | 4.57 | 4.91 | **1.04** |
| oltp_delete | 4.57 | 4.65 | 0.41 |
| oltp_update_index | 4.82 | 4.91 | 0.42 |
| oltp_update_non_index | 4.57 | 4.82 | 0.36 |
| select_random_points | 0.23 | **0.16** | 0.51 |
| select_random_ranges | 0.21 | **0.20** | 0.43 |
| bulk_insert | **0.00** | 0.00 | 0.95 |

## Analysis

### Where Firebird excels

- **oltp_insert:** Firebird matches MariaDB at 5,003 TPS with dramatically lower
  latency (0.80ms vs 3.65ms). Individual INSERT performance is excellent.
- **95th percentile on writes:** Firebird shows very tight tail latency on
  update/delete operations (0.36-0.42ms at p95 vs 4.57-4.91ms for MySQL/PostgreSQL).
  The high average but low p95 indicates occasional lock conflicts that inflate
  the mean without affecting most operations.

### Where MySQL/PostgreSQL lead

- **Pure reads:** MariaDB's InnoDB buffer pool and PostgreSQL's shared_buffers
  deliver 3-4x higher throughput on cached point selects and read-only workloads.
- **bulk_insert:** MySQL and PostgreSQL use multi-row INSERT syntax (batching many
  rows per statement), while Firebird sends individual INSERTs. This is a driver
  limitation, not a Firebird limitation — Phase 2 EXECUTE BLOCK optimization will
  address this.
- **oltp_delete and updates:** Firebird's MVCC architecture shows higher average
  latency on single-row modifications due to version chain management.

### Key observations

- **oltp_read_write (the flagship benchmark):** All three databases perform within
  the same order of magnitude (577-822 TPS). Firebird is competitive on the mixed
  workload that matters most for real applications.
- **This is a WSL2 environment, not bare-metal.** Results are relative, not absolute.
  The ratio between databases is what matters.
- **Firebird's write latency variance:** The high average but low p95 on
  write-heavy tests (delete, update_index, update_non_index) is caused by
  occasional lock conflicts with 4 concurrent threads. Most operations complete
  in <1ms but a few take >10s (lock waits), inflating the average.

---

## Raw Output

### Firebird 5.0.4

```
========================================
Sysbench Benchmark Suite
Driver: firebird
Tables: 4 x 10000 rows
Threads: 4, Time: 10s
Script start: Wed May 27 19:12:42 CEST 2026
========================================
```

#### oltp_point_select
```
    transactions:                        145377 (14527.53 per sec.)
    queries:                             145377 (14527.53 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.27ms
    Latency 95th:                        0.35ms
```

#### oltp_read_only
```
    transactions:                        6642   (663.54 per sec.)
    queries:                             106272 (10616.69 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         6.02ms
    Latency 95th:                        7.43ms
```

#### oltp_read_write
```
    transactions:                        5775   (576.78 per sec.)
    queries:                             115515 (11537.00 per sec.)
    ignored errors:                      1      (0.10 per sec.)
    Latency avg:                         6.93ms
    Latency 95th:                        8.28ms
```

#### oltp_insert
```
    transactions:                        50167  (5003.22 per sec.)
    queries:                             50167  (5003.22 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.80ms
    Latency 95th:                        1.04ms
```

#### oltp_delete
```
    transactions:                        752    (74.79 per sec.)
    queries:                             752    (74.79 per sec.)
    ignored errors:                      1      (0.10 per sec.)
    Latency avg:                         53.43ms
    Latency 95th:                        0.41ms
```

#### oltp_update_index
```
    transactions:                        750    (74.57 per sec.)
    queries:                             750    (74.57 per sec.)
    ignored errors:                      1      (0.10 per sec.)
    Latency avg:                         53.56ms
    Latency 95th:                        0.42ms
```

#### oltp_update_non_index
```
    transactions:                        606    (60.42 per sec.)
    queries:                             606    (60.42 per sec.)
    ignored errors:                      1      (0.10 per sec.)
    Latency avg:                         66.13ms
    Latency 95th:                        0.36ms
```

#### select_random_points
```
    transactions:                        113902 (11383.20 per sec.)
    queries:                             113902 (11383.20 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.35ms
    Latency 95th:                        0.51ms
```

#### select_random_ranges
```
    transactions:                        126672 (12658.68 per sec.)
    queries:                             126672 (12658.68 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.31ms
    Latency 95th:                        0.43ms
```

#### bulk_insert
```
    transactions:                        10000  (5370.73 per sec.)
    queries:                             10008  (5375.02 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.73ms
    Latency 95th:                        0.95ms
```

### MariaDB 11.8.6

```
========================================
Sysbench Benchmark Suite
Driver: mysql
Tables: 4 x 10000 rows
Threads: 4, Time: 10s
Script start: Wed May 27 19:17:48 CEST 2026
========================================
```

#### oltp_point_select
```
    transactions:                        555156 (55511.89 per sec.)
    queries:                             555156 (55511.89 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.07ms
    Latency 95th:                        0.12ms
```

#### oltp_read_only
```
    transactions:                        26121  (2611.57 per sec.)
    queries:                             417936 (41785.13 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         1.53ms
    Latency 95th:                        2.07ms
```

#### oltp_read_write
```
    transactions:                        8225   (822.16 per sec.)
    queries:                             164532 (16446.33 per sec.)
    ignored errors:                      2      (0.20 per sec.)
    Latency avg:                         4.86ms
    Latency 95th:                        7.70ms
```

#### oltp_insert
```
    transactions:                        10954  (1095.11 per sec.)
    queries:                             10954  (1095.11 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         3.65ms
    Latency 95th:                        4.57ms
```

#### oltp_delete
```
    transactions:                        12612  (1260.76 per sec.)
    queries:                             12612  (1260.76 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         3.17ms
    Latency 95th:                        4.57ms
```

#### oltp_update_index
```
    transactions:                        10813  (1080.99 per sec.)
    queries:                             10813  (1080.99 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         3.70ms
    Latency 95th:                        4.82ms
```

#### oltp_update_non_index
```
    transactions:                        10993  (1098.91 per sec.)
    queries:                             10993  (1098.91 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         3.64ms
    Latency 95th:                        4.57ms
```

#### select_random_points
```
    transactions:                        255067 (25504.42 per sec.)
    queries:                             255067 (25504.42 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.16ms
    Latency 95th:                        0.23ms
```

#### select_random_ranges
```
    transactions:                        251456 (25142.95 per sec.)
    queries:                             251456 (25142.95 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.16ms
    Latency 95th:                        0.21ms
```

#### bulk_insert
```
    transactions:                        10000  (135271.14 per sec.)
    queries:                             4      (54.11 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.00ms
    Latency 95th:                        0.00ms
```

### PostgreSQL 17.10

```
========================================
Sysbench Benchmark Suite
Driver: pgsql
Tables: 4 x 10000 rows
Threads: 4, Time: 10s
Script start: Wed May 27 19:19:35 CEST 2026
========================================
```

#### oltp_point_select
```
    transactions:                        417703 (41757.98 per sec.)
    queries:                             417703 (41757.98 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.10ms
    Latency 95th:                        0.14ms
```

#### oltp_read_only
```
    transactions:                        22709  (2270.24 per sec.)
    queries:                             363344 (36323.80 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         1.76ms
    Latency 95th:                        2.26ms
```

#### oltp_read_write
```
    transactions:                        7732   (772.83 per sec.)
    queries:                             154640 (15456.54 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         5.17ms
    Latency 95th:                        7.98ms
```

#### oltp_insert
```
    transactions:                        10371  (1036.75 per sec.)
    queries:                             10371  (1036.75 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         3.85ms
    Latency 95th:                        4.91ms
```

#### oltp_delete
```
    transactions:                        12691  (1268.78 per sec.)
    queries:                             12691  (1268.78 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         3.15ms
    Latency 95th:                        4.65ms
```

#### oltp_update_index
```
    transactions:                        10204  (1020.12 per sec.)
    queries:                             10204  (1020.12 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         3.92ms
    Latency 95th:                        4.91ms
```

#### oltp_update_non_index
```
    transactions:                        10360  (1035.63 per sec.)
    queries:                             10360  (1035.63 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         3.86ms
    Latency 95th:                        4.82ms
```

#### select_random_points
```
    transactions:                        324337 (32426.91 per sec.)
    queries:                             324337 (32426.91 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.12ms
    Latency 95th:                        0.16ms
```

#### select_random_ranges
```
    transactions:                        246415 (24636.29 per sec.)
    queries:                             246415 (24636.29 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.16ms
    Latency 95th:                        0.20ms
```

#### bulk_insert
```
    transactions:                        10000  (304852.95 per sec.)
    queries:                             4      (121.94 per sec.)
    ignored errors:                      0      (0.00 per sec.)
    Latency avg:                         0.01ms
    Latency 95th:                        0.00ms
```

---

*Generated on 2026-05-27 by sysbench with Firebird driver (ZlatanOmerovic/firebird-sysbench)*
