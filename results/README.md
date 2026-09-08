# Raw benchmark output

Unedited `run_benchmarks.sh` / `run_benchmarks_readonly.sh` output, kept as
the evidence trail behind the `BENCHMARK_*.md` documents in the repository
root. Directories are named after the date in the filenames.

| Directory | Files | Round |
|---|---|---|
| `2026-05-27` | 12 | First measurements — **superseded** |
| `2026-05-28` | 15 | Second measurements — **superseded** |
| `2026-06-16` | 18 | The published round |

## Two caveats before quoting anything here

**The headers of the runs archived here do not record which server was
measured.** A run's header has
the driver name, table count, table size, thread count, duration and a
timestamp — but not the connection string, port, `libfbclient` path or
buffer-cache setting. Nothing inside a `benchmark_firebird_*.txt` file
distinguishes a Firebird 4, 5 or 6 run. The attribution table below was
reconstructed by matching `oltp_point_select` throughput against the
published tables. Runs from 2026-09-08 onward record the connection string
(password redacted) and the resolved `libfbclient` path in the header, so
this reconstruction is only needed for the files already here.

**Every file here is a `4 tables x 10,000 rows` run.** The 1M-row scaling
tables in `BENCHMARK_RESULTS.md`,
`BENCHMARK_FIREBIRD_VERSIONS.md`, `BENCHMARK_FIREBIRD_6_vs_5.md` and
`BENCHMARK_FIREBIRD6_vs_MYSQL_PGSQL.md` — including the headline FB6 read
gains and the FB6 `oltp_read_write` collapse — were run by hand outside the
script and have no raw counterpart in this directory.

## Attribution (2026-06-16)

Matched on `oltp_point_select` TPS, which is unique per run:

| File | Engine | Published in |
|---|---|---|
| `benchmark_firebird_20260616_155429.txt` | Firebird 4.0.7 (14,824) | `BENCHMARK_FIREBIRD_VERSIONS.md` |
| `benchmark_firebird_20260616_124647.txt` | Firebird 5.0.4 (14,951) | `BENCHMARK_FIREBIRD_VERSIONS.md`, `BENCHMARK_FIREBIRD_6_vs_5.md` |
| `benchmark_firebird_20260616_144037.txt` | Firebird 6.0.0.2012 (14,374) | `BENCHMARK_FIREBIRD_VERSIONS.md`, `BENCHMARK_FIREBIRD_6_vs_5.md` |
| `benchmark_firebird_20260616_165649.txt` | Firebird 5.0.4 (14,835) | `BENCHMARK_RESULTS.md` |
| `benchmark_firebird_20260616_164728.txt` | Firebird 6.0.0.2012 (14,831) | `BENCHMARK_FIREBIRD6_vs_MYSQL_PGSQL.md` |
| `benchmark_mysql_20260616_164402.txt` | MariaDB 11.8.6 (56,498) | both three-way documents |
| `benchmark_pgsql_20260616_164551.txt` | PostgreSQL 17.10 (42,731) | both three-way documents |

The remaining 2026-06-16 files are intermediate runs from the same day:
`112017`, `123700`, `124057`, `134111`, `134342` (Firebird), `112246`
(MariaDB) and `112428` (PostgreSQL). Four Firebird files — `134027`,
`134438`, `161715`, `162306` — contain only a header: the suite aborted at
the initial cleanup step before any benchmark ran.

## Why the earlier rounds are superseded

The 2026-05-27 and 2026-05-28 numbers were measured before three fairness
fixes, so Firebird's figures there are not comparable with MariaDB's or
PostgreSQL's:

1. The Firebird database lived on tmpfs (`/tmp`) while the other engines
   were on disk.
2. Buffer caches were not matched across engines.
3. The driver held one long transaction instead of committing per statement,
   so inserts paid no fsync.

They are kept as a record of what those fixes changed — see "What we fixed
for fair comparison" in `BENCHMARK_RESULTS.md`.
