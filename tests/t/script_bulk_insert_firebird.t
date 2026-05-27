########################################################################
bulk_insert.lua + Firebird tests
########################################################################

  $ . $SBTEST_INCDIR/firebird_common.sh
  $ SCRIPT="${SBTEST_SCRIPTDIR}/bulk_insert.lua"
  $ COMMON="$DB_DRIVER_ARGS --threads=2"

  $ sysbench $SCRIPT $COMMON --verbosity=1 cleanup >/dev/null 2>&1 || true

  $ sysbench $SCRIPT $COMMON --verbosity=1 prepare
  Creating table 'sbtest1'...
  Creating table 'sbtest2'...

  $ sysbench $SCRIPT $COMMON --events=100 run | grep 'transactions:\|ignored errors:'
      transactions: .* (re)
      ignored errors: .* (re)

  $ sysbench $SCRIPT $COMMON --verbosity=1 cleanup
  Dropping table 'sbtest1'...
  Dropping table 'sbtest2'...
