# sysbench — Firebird Edition

Fork of [akopytov/sysbench](https://github.com/akopytov/sysbench) with native
Firebird database support. Run the same industry-standard OLTP benchmarks used
for MySQL and PostgreSQL against Firebird 3, 4, and 5.

## Quick Start

```bash
# Build
./autogen.sh
./configure --with-firebird=/opt/firebird
make -j$(nproc)

# Create a test database (using Firebird's isql)
isql -user SYSDBA -password masterkey <<< "CREATE DATABASE 'localhost:/tmp/sbtest.fdb' USER 'SYSDBA' PASSWORD 'masterkey' DEFAULT CHARACTER SET UTF8; QUIT;"

# Prepare, run, cleanup
./src/sysbench ./src/lua/oltp_read_write.lua \
  --db-driver=firebird \
  --firebird-db=localhost:/tmp/sbtest.fdb \
  --firebird-user=SYSDBA \
  --firebird-password=masterkey \
  --tables=4 --table-size=10000 prepare

./src/sysbench ./src/lua/oltp_read_write.lua \
  --db-driver=firebird \
  --firebird-db=localhost:/tmp/sbtest.fdb \
  --firebird-user=SYSDBA \
  --firebird-password=masterkey \
  --tables=4 --table-size=10000 --threads=4 --time=10 run

./src/sysbench ./src/lua/oltp_read_write.lua \
  --db-driver=firebird \
  --firebird-db=localhost:/tmp/sbtest.fdb \
  --firebird-user=SYSDBA \
  --firebird-password=masterkey \
  --tables=4 cleanup
```

## Firebird Driver Options

```
--firebird-db        Database connection string (e.g., localhost:/tmp/sbtest.fdb)
--firebird-user      User name (default: SYSDBA)
--firebird-password  Password (default: masterkey)
```

For non-default ports: `--firebird-db=localhost/3054:/tmp/sbtest.fdb`

## Supported Benchmarks

All standard sysbench OLTP benchmarks work with Firebird:

| Script | Description |
|---|---|
| `oltp_point_select` | Single-row SELECT by primary key |
| `oltp_read_only` | Read-only mix: point selects, range scans, SUM, ORDER BY, DISTINCT |
| `oltp_read_write` | Mixed read/write OLTP workload (the flagship benchmark) |
| `oltp_insert` | INSERT-only workload |
| `oltp_delete` | DELETE-only workload |
| `oltp_update_index` | UPDATE on indexed column |
| `oltp_update_non_index` | UPDATE on non-indexed column |
| `select_random_points` | SELECT with random IN() clause |
| `select_random_ranges` | SELECT with random BETWEEN ranges |
| `bulk_insert` | Bulk INSERT throughput |

## Supported Firebird Versions

The driver uses the legacy ISC API (`ibase.h`) which is stable across all
modern Firebird versions. Tested and verified against:

- **Firebird 5.0.4** — full support, all tests pass
- **Firebird 4.0.7** — full support, all tests pass
- **Firebird 3.0.14** — full support, all benchmarks pass

## Building with Multiple Drivers

sysbench can be built with MySQL, PostgreSQL, and Firebird simultaneously:

```bash
./configure --with-mysql --with-pgsql --with-firebird=/opt/firebird
make -j$(nproc)
```

This produces a single binary that supports all three databases, enabling
direct A/B/C comparisons on identical hardware.

## Benchmark Scripts

Two helper scripts are included for running complete benchmark suites:

```bash
# Full suite (10 benchmarks, cleanup+prepare before each)
./run_benchmarks.sh firebird
./run_benchmarks.sh mysql
./run_benchmarks.sh pgsql

# Read-safe suite (5 benchmarks, single prepare)
./run_benchmarks_readonly.sh firebird

# Custom Firebird connection
FIREBIRD_DB=localhost/3054:/tmp/sbtest.fdb ./run_benchmarks.sh firebird

# Custom parameters
THREADS=8 TIME=30 TABLE_SIZE=100000 ./run_benchmarks.sh firebird
```

Results are saved to timestamped files: `benchmark_<driver>_<timestamp>.txt`

## Documentation

| Document | Description |
|---|---|
| [FIREBIRD.md](FIREBIRD.md) | Driver specification and post-MVP roadmap |
| [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) | Three-way comparison: MariaDB vs PostgreSQL vs Firebird |
| [BENCHMARK_FIREBIRD_VERSIONS.md](BENCHMARK_FIREBIRD_VERSIONS.md) | Cross-version comparison: Firebird 3 vs 4 vs 5 |
| [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md) | What was changed in sysbench, architecture decisions |
| [docs/DEV_ENVIRONMENT.md](docs/DEV_ENVIRONMENT.md) | Development machine setup (WSL2, Debian, tools) |
| [docs/BUILDING_FIREBIRD.md](docs/BUILDING_FIREBIRD.md) | Compiling Firebird 3, 4, and 5 from source |

## Tests

The Firebird driver includes 11 test files using the
[Cram](https://bitheap.org/cram/) framework, matching the coverage of the
MySQL and PostgreSQL drivers:

```bash
export SBTEST_FIREBIRD_ARGS="--firebird-db=localhost:/tmp/sbtest.fdb --firebird-user=SYSDBA --firebird-password=masterkey"
cd tests
./test_run.sh t/drv_firebird.t
./test_run.sh t/api_sql_firebird.t
./test_run.sh t/script_oltp_read_write_firebird.t
# ... etc
```

## Upstream Compatibility

This fork maintains byte-identical Lua workload scripts with upstream sysbench.
The only changes to upstream files are additive:

- `configure.ac` — `--with-firebird` option (23 lines)
- `src/db_driver.h` / `src/db_driver.c` — driver registration (7 lines)
- `src/Makefile.am` / `src/drivers/Makefile.am` — build wiring (10 lines)
- `src/db_driver.c` — single-row bulk insert support (10 lines)
- `src/lua/oltp_common.lua` — Firebird DDL branch (7 lines)
- `src/lua/oltp_insert.lua` — Firebird auto-inc support (1 line)

SQL dialect differences (transactions, DDL syntax) are handled inside the
driver, never in workload scripts. Verify with:

```bash
git diff upstream/master -- src/lua/
```

## License

GPLv2, same as upstream sysbench.

## Links

- **This fork:** https://github.com/ZlatanOmerovic/firebird-sysbench
- **Upstream sysbench:** https://github.com/akopytov/sysbench
- **Firebird SQL:** https://firebirdsql.org
