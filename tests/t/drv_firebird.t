########################################################################
Firebird driver tests
########################################################################

  $ . $SBTEST_INCDIR/firebird_common.sh

  $ cat >test.lua <<'EOF'
  > function event()
  >   c = c or sysbench.sql.driver():connect()
  >   c:query("SELECT 1 FROM RDB$DATABASE")
  > end
  > EOF

  $ sysbench test.lua --events=10 --threads=2 $DB_DRIVER_ARGS run | head -n 42
  sysbench * (glob)
  \s* (re)
  Running the test with following options:
  Number of threads: 2
  Initializing random number generator from current time
  \s* (re)
  \s* (re)
  Initializing worker threads...
  \s* (re)
  Threads started!
  \s* (re)
  SQL statistics:
      queries performed:
          read:                            10
          write:                           0
          other:                           0
          total:                           10
      transactions: .* (re)
      queries: .* (re)
      ignored errors: .* (re)
      reconnects: .* (re)
  \s* (re)
  Throughput:
      events/s .* (re)
      time elapsed: .* (re)
      total number of events:              10
  \s* (re)
  Latency .* (re)
  .* min: .* (re)
  .* avg: .* (re)
  .* max: .* (re)
  .* 95th percentile: .* (re)
  .* sum: .* (re)
  \s* (re)
  Threads fairness:
      events .* (re)
      execution time .* (re)
  \s* (re)
