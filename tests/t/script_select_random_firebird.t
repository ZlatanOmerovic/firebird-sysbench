########################################################################
select_random_points.lua + select_random_ranges.lua + Firebird tests
########################################################################

  $ . $SBTEST_INCDIR/firebird_common.sh

########################################################################
# select_random_points
########################################################################
  $ ARGS="${SBTEST_SCRIPTDIR}/select_random_points.lua $DB_DRIVER_ARGS --tables=1 --table-size=1000"

  $ sysbench $ARGS --verbosity=1 prepare
  Creating table 'sbtest1'...
  Inserting 1000 records into 'sbtest1'
  Creating a secondary index on 'sbtest1'...

  $ sysbench $ARGS --events=100 run | grep 'transactions:\|ignored errors:'
      transactions: .* (re)
      ignored errors: .* (re)

  $ sysbench $ARGS --verbosity=1 cleanup
  Dropping table 'sbtest1'...

########################################################################
# select_random_ranges
########################################################################
  $ ARGS="${SBTEST_SCRIPTDIR}/select_random_ranges.lua $DB_DRIVER_ARGS --tables=1 --table-size=1000"

  $ sysbench $ARGS --verbosity=1 prepare
  Creating table 'sbtest1'...
  Inserting 1000 records into 'sbtest1'
  Creating a secondary index on 'sbtest1'...

  $ sysbench $ARGS --events=100 run | grep 'transactions:\|ignored errors:'
      transactions: .* (re)
      ignored errors: .* (re)

  $ sysbench $ARGS --verbosity=1 cleanup
  Dropping table 'sbtest1'...
