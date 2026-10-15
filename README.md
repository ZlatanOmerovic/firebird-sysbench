# sysbench — Firebird Edition

Fork of [akopytov/sysbench](https://github.com/akopytov/sysbench) with native
Firebird database support. Run the same industry-standard OLTP benchmarks used
for MySQL and PostgreSQL against Firebird 4, 5, and 6.

The driver uses the **Firebird OO API** through its C wrapper (`fb_c_api.h`),
not the legacy ISC API — OO API calls go straight to the engine instead of
through the yvalve ISC translation layer. On Firebird 4 and newer the
**Batch API** loads bulk data with one round trip per batch of rows instead of
one per row.

`libfbclient.so` is loaded at runtime with `dlopen`, so a single sysbench
binary can drive different Firebird versions — point `--firebird-client` at
the client library you want.

The legacy ISC API driver is preserved on the `firebird-isc` branch; it is the
only option for Firebird 3 (see [Supported Firebird
Versions](#supported-firebird-versions)).

## Quick Start

Building needs headers from **Firebird 5 or newer** — `fb_c_api.h` first
shipped in FB5, and FB3/FB4 install only the C++ interface headers. The
resulting binary still connects to FB4 servers at runtime.

```bash
# Build (--with-firebird must point at an FB5+ installation)
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

`/tmp` is tmpfs on many systems, so the example above measures an in-memory
database. For anything you intend to compare against MariaDB or PostgreSQL,
put the `.fdb` on the same disk as their data directories — see
[docs/BUILDING_FIREBIRD.md](docs/BUILDING_FIREBIRD.md#create-test-databases).

## Firebird Driver Options

```
--firebird-db        Database connection string (e.g., localhost:/tmp/sbtest.fdb)
--firebird-user      User name (default: SYSDBA)
--firebird-password  Password (default: masterkey)
--firebird-client    libfbclient.so to dlopen at runtime (default: libfbclient.so,
                     resolved through the normal loader search path)
```

For non-default ports: `--firebird-db=localhost/3054:/tmp/sbtest.fdb`

To benchmark another Firebird version with the same binary, give it that
version's client library:

```bash
--firebird-client=/opt/firebird4/lib/libfbclient.so \
--firebird-db=localhost/3054:/var/lib/firebird4/sbtest.fdb
```

A connection string with no `host:` or `host/port:` prefix makes libfbclient
load the **embedded** engine in-process. That works, but the embedded engine
needs a bigger stack than sysbench's 64K default — add
`--thread-stack-size=2M` or worker threads will crash inside `attachDatabase`.
The driver warns when it detects this.

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

| Version | As server | As client library | Batch API |
|---|---|---|---|
| **Firebird 6.0** (trunk) | Tested | Ships `fb_c_api.h` — works | Yes |
| **Firebird 5.0** | Tested — reference platform | Ships `fb_c_api.h` — works | Yes |
| **Firebird 4.0** | Tested | Works (no `fb_c_api.h`, so it can't be built against) | Yes |
| **Firebird 3.0** | Untested | Refused at startup | No |

The driver probes the runtime `IUtil` vtable version in `drv_init`:

- **below v3** — fatal error. FB3's client reports **v2**: its OO API vtable
  layout predates the methods this driver dispatches through, so calling
  them would crash. The message points at the `firebird-isc` branch.
- **below v4** — accepted, but the Batch API is switched off and bulk
  inserts fall back to single-row, because `IStatement::createBatch` does
  not exist that far back. This is a safety net rather than a version
  statement: FB4.0.7's client already reports v4 or newer and keeps
  batching, so no client tested here lands in this branch.
- **v4 and up** — everything enabled. FB4, FB5 and FB6 clients all qualify.

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
| [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) | Three-way: MariaDB vs PostgreSQL vs Firebird 5.0 |
| [BENCHMARK_FIREBIRD6_vs_MYSQL_PGSQL.md](BENCHMARK_FIREBIRD6_vs_MYSQL_PGSQL.md) | Three-way: MariaDB vs PostgreSQL vs Firebird 6.0 |
| [BENCHMARK_FIREBIRD_VERSIONS.md](BENCHMARK_FIREBIRD_VERSIONS.md) | Cross-version: Firebird 4 vs 5 vs 6 |
| [BENCHMARK_FIREBIRD_6_vs_5.md](BENCHMARK_FIREBIRD_6_vs_5.md) | Head-to-head: Firebird 6.0 vs 5.0 |
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
