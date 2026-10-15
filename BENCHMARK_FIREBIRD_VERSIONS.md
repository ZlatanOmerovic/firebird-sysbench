# Sysbench Benchmark Results — Firebird Cross-Version (FB4 / FB5 / FB6)

> **Note on these results (added 2026-10-15).** They were measured with
> the driver of that time: Firebird ran every SELECT outside BEGIN in one
> long read-write snapshot transaction, and each SELECT used `openCursor` +
> `fetch`. The current driver uses a read-only read committed transaction
> and a one-row execute for SELECT (see FIREBIRD.md).

## Environment

- **OS:** Debian 13 (trixie) on WSL2, Linux 6.6.87, x86_64
- **CPU:** AMD (host CPU via WSL2)
- **sysbench:** 1.1.0-844b13c (OO API driver with Batch API + autocommit + dlopen)
- **All three** with `DefaultDbCachePages = 16384` (128 MB), page size 8 KB
- **All three** running on disk under `/var/lib/firebird{,4,6}/sbtest.fdb`
- **Date:** 2026-06-16

| Firebird | Build | Port | Notes |
|---|---|---|---|
| 3.0.14 | release | 3057 | gcc 14, driven by the FB5 client |
| 4.0.7 | release | 3055 | gcc 14 |
| 5.0.4 | release | 3050 | gcc 14 |
| 6.0.0.2012 | Trunk / Initial | 3056 | gcc 14, master branch HEAD |

Ports differ from earlier rounds: 3053 and 3054 were occupied by unrelated
Firebird servers on the Windows host, visible inside WSL2 through mirrored
networking. Identify a server by attaching and reading
`rdb$get_context('SYSTEM','ENGINE_VERSION')`, not by reading `ss` output.

### Firebird 3 — server works, client library does not

Earlier rounds excluded FB3 entirely. That was too broad: the restriction is
on the **client library**, not the server.

Loading FB3's `libfbclient.so` is refused at startup, because its OO API
vtable predates methods this driver dispatches through:

```
FATAL: Firebird client library is too old for this driver (IUtil vtable v2).
       The OO API driver requires libfbclient from Firebird 4.0 or newer.
       For Firebird 3 support, use the `firebird-isc` branch which uses
       the legacy ISC API.
```

An FB3 **server** driven by the FB5 client is a different matter, and works:
it attaches, prepares, and runs the full suite. FB3 is therefore included in
the tables below, with one structural difference — the Batch API is a
Firebird 4+ server feature, so bulk loading falls back to single-row inserts.
That shows up directly in `bulk_insert`: 5,614 TPS against 39–45K for FB4/5/6.

## Prepare phase — 4 tables × 1,000,000 rows (Batch API)

| Version | Wall time |
|---|---|
| Firebird 4.0.7 | 15 min 46 sec |
| Firebird 5.0.4 | ~15 min |
| **Firebird 6.0.0.2012** | **28 min 02 sec** (~87% slower) |

FB6 master shows a notable regression on the Batch API path.

## TABLE_SIZE=10K (sysbench default), 4 threads, TPS

| Benchmark | FB 3.0.14 | FB 4.0.7 | FB 5.0.4 | FB 6.0.0.2012 |
|---|---|---|---|---|
| oltp_point_select | 14,080 | 15,233 | 14,510 | **15,331** |
| oltp_read_only | 522 | 549 | 535 | **563** |
| oltp_read_write | 54 | 54 | 55 | **59** |
| oltp_insert | 148 | 147 | **153** | 149 |
| oltp_delete | 56 | **286** | 66 | 94 |
| oltp_update_index | 137 | **139** | 55 | 48 |
| oltp_update_non_index | 181 | **189** | 51 | 62 |
| select_random_points | **11,795** | 11,512 | 11,021 | 11,752 |
| select_random_ranges | **12,033** | 11,916 | 11,810 | 10,984 |
| bulk_insert | 5,614 | **46,285** | 39,585 | 45,086 |

Measured 2026-10-14, second of two consecutive runs, after the
`query()`-insert batching fix. `oltp_insert` was previously inflated ~27×
across every version by that path — see *Correction* in
`BENCHMARK_RESULTS.md`.

**The write rows do not support version rankings.** Repeating this suite
back to back moved `oltp_delete` and `oltp_update_*` by 0.37–1.42× on the
same engine and build. FB3's `oltp_delete` is the clearest case: 149 TPS in
the first pass, 56 in the second. Only `bulk_insert` (FB3's single-row
fallback) and the read benchmarks are stable enough to compare across
versions here.

At 10K the dataset fits in cache. Differences are mostly noise; the
notable signal is that **FB4 is fastest on the update/delete benchmarks**
(165/260/239 vs ~130/150/240 on FB5/6) — possibly a real regression
introduced after FB4.

## TABLE_SIZE=1M, scaling 1 → 2 → 4 threads, TPS

> **Not re-measured.** The figures in this section predate the
> `query()`-insert batching fix and have not been repeated at 1M rows. The
> read rows are unaffected by that fix, but were single runs, and Firebird's
> write rows carry the run-to-run variance described above.


| Benchmark | FB | 1t | 2t | 4t |
|---|---|---|---|---|
| **oltp_point_select** | FB4 | 5,957 | 10,105 | 14,342 |
| | FB5 | 2,276 | 5,266 | 9,428 |
| | **FB6** | 3,968 | 9,323 | **14,382** |
| **oltp_read_only** | FB4 | 137 | 296 | 537 |
| | FB5 | 69 | 143 | 195 |
| | **FB6** | 131 | 291 | **566** |
| **oltp_read_write** | FB4 | 27 | 37 | **46** |
| | FB5 | 20 | 29 | 44 |
| | FB6 | 31 | 40 | 23 |
| **select_random_points** | FB4 | 2,704 | 5,281 | 8,368 |
| | FB5 | 645 | 1,396 | 2,837 |
| | **FB6** | 2,646 | 5,291 | **10,502** |
| **select_random_ranges** | FB4 | 2,366 | 2,751 | 1,510 |
| | FB5 | 143 | 307 | 613 |
| | **FB6** | **2,177** | **4,016** | **3,985** |
| **oltp_update_index** | FB4 | 68 | 94 | **130** |
| | FB5 | 63 | 47 | 45 |
| | FB6 | 69 | 95 | 107 |
| **oltp_update_non_index** | FB4 | 106 | 133 | **169** |
| | FB5 | 36 | 50 | 66 |
| | FB6 | 106 | 82 | 85 |
| **oltp_delete** | FB4 | **101** | **144** | 140 |
| | FB5 | 45 | 61 | 81 |
| | FB6 | 53 | 60 | 77 |

## Headlines

### FB5 looks worst on disk at 1M

The biggest revelation: **FB5 is roughly half the speed of both FB4 and
FB6** across most 1M workloads on disk. This contradicts our earlier
tmpfs/non-fair results, where FB5 looked competitive. Specifically:

- `oltp_point_select` @ 4t: FB4 14,342 vs FB5 **9,428** vs FB6 14,382
- `oltp_read_only` @ 4t: FB4 537 vs FB5 **195** vs FB6 566
- `select_random_ranges` @ 1t: FB4 2,366 vs FB5 **143** vs FB6 2,177
  (FB5 is **16× slower** than the others on range scans at 1 thread)

This is striking enough that it warrants independent verification — but
the runs were back-to-back on the same disk, same driver, with FB5 and FB6
on the same 128 MB cache config, identical sysbench options.

### FB6 reverses two FB5 regressions, introduces one new one

- **`select_random_ranges` recovered**: FB5 was 143-613 TPS, FB6 climbs
  back to 2,177-3,985 — close to FB4's level (2,366-1,510).
- **`oltp_read_only`, `select_random_points`, `point_select`** all
  recovered from FB5's slump.
- **New regression in FB6**: `oltp_read_write` at 4 threads drops from 44
  (FB5) → 23 (FB6). 1 and 2 threads are fine; 4 threads regresses
  sharply. Worth flagging to Dmitry.

### FB4 is the best release for writes

- `oltp_update_index`/`oltp_update_non_index`/`oltp_delete` at 4 threads:
  FB4 130/169/140 vs FB5 45/66/81 vs FB6 107/85/77.
- FB4 is the only version where MORE threads consistently increase write
  throughput on the 1M dataset; FB5 stays flat, FB6 is mixed.

### Bulk insert regression in FB6 (Batch API)

FB6's 4M-row prepare took **28 minutes** vs FB4's 15:46. That's a real
regression on the Batch API path — likely fixable before FB6 release.

## Methodology notes

- Each cell is a single 10-second run. Variance not characterised; treat
  ±10% as noise.
- All databases on the same physical disk (`/dev/sdd`, ext4) with same
  mount options.
- The driver matches MySQL/PostgreSQL semantics: autocommit per
  statement when no explicit BEGIN was sent.
- `--firebird-client=PATH` was not used; each version was tested via
  `LD_LIBRARY_PATH=/opt/firebird{4,5,6}/lib` so the runtime library
  matches the server we connect to.
- sysbench default `--thread-stack-size=64K` was used (networked mode
  works fine; embedded mode would need 2M).
- WSL2 host. Absolute numbers transferable only by ratio.

*Generated on 2026-06-16 by sysbench with Firebird OO API driver*
