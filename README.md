# sysbench — Firebird Edition

Fork of [akopytov/sysbench](https://github.com/akopytov/sysbench) with native
Firebird database support. Run the same industry-standard OLTP benchmarks used
for MySQL and PostgreSQL against Firebird 3, 4, and 5.

The driver uses the **Firebird OO API** (`fb_c_api.h`) — the native interface
in Firebird 3+. On Firebird 4+, the **Batch API** is used for bulk inserts,
delivering 50x faster data loading than single-row INSERTs. The legacy ISC API
version is preserved on the `firebird-isc` branch.

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

| Version | Support | Batch API |
|---|---|---|
| **Firebird 5.0** | Full | Yes (313K TPS bulk insert) |
| **Firebird 4.0** | Full | Yes (321K TPS bulk insert) |
| **Firebird 3.0** | Full | No (falls back to single-row, 6K TPS) |

## Building with Multiple Drivers

```bash
./configure --with-mysql --with-pgsql --with-firebird=/opt/firebird
make -j$(nproc)
```

## Benchmark Scripts

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

## Documentation

| Document | Description |
|---|---|
| [FIREBIRD.md](FIREBIRD.md) | Driver specification and roadmap |
| [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) | Three-way: MariaDB vs PostgreSQL vs Firebird |
| [BENCHMARK_FIREBIRD_VERSIONS.md](BENCHMARK_FIREBIRD_VERSIONS.md) | Cross-version: Firebird 3 vs 4 vs 5 |
| [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md) | Implementation details |
| [docs/DEV_ENVIRONMENT.md](docs/DEV_ENVIRONMENT.md) | Development machine setup |
| [docs/BUILDING_FIREBIRD.md](docs/BUILDING_FIREBIRD.md) | Compiling Firebird from source |

## Tests

11 Cram test files matching MySQL/PostgreSQL coverage:

```bash
export SBTEST_FIREBIRD_ARGS="--firebird-db=localhost:/tmp/sbtest.fdb --firebird-user=SYSDBA --firebird-password=masterkey"
cd tests && ./test_run.sh t/drv_firebird.t
```

## Upstream Compatibility

Lua workload scripts stay byte-identical to upstream. Only additive changes:

- `configure.ac` — `--with-firebird` option
- `src/db_driver.h` / `src/db_driver.c` — driver registration + bulk insert fix
- `src/Makefile.am` / `src/drivers/Makefile.am` — build wiring
- `src/lua/oltp_common.lua` — Firebird DDL branch
- `src/lua/oltp_insert.lua` — Firebird auto-inc support

## License

GPLv2, same as upstream sysbench.

## Links

- **This fork:** https://github.com/ZlatanOmerovic/firebird-sysbench
- **Upstream sysbench:** https://github.com/akopytov/sysbench
- **Firebird SQL:** https://firebirdsql.org
