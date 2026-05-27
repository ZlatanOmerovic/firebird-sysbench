########################################################################
oltp_point_select.lua + Firebird tests
########################################################################

  $ . $SBTEST_INCDIR/firebird_common.sh
  $ OLTP_SCRIPT_PATH=${SBTEST_SCRIPTDIR}/oltp_point_select.lua

########################################################################
# Prepare + run + cleanup (single thread)
########################################################################
  $ sysbench ${OLTP_SCRIPT_PATH} ${DB_DRIVER_ARGS} --tables=2 --table-size=1000 --threads=1 prepare
  sysbench .* (re)
  \s* (re)
  Creating table 'sbtest1'...
  Inserting 1000 records into 'sbtest1'
  Creating a secondary index on 'sbtest1'...
  Creating table 'sbtest2'...
  Inserting 1000 records into 'sbtest2'
  Creating a secondary index on 'sbtest2'...

  $ sysbench ${OLTP_SCRIPT_PATH} ${DB_DRIVER_ARGS} --tables=2 --table-size=1000 --threads=1 --events=100 run | grep 'transactions:\|ignored errors:'
      transactions: .* (re)
      ignored errors: .* (re)

  $ sysbench ${OLTP_SCRIPT_PATH} ${DB_DRIVER_ARGS} --tables=2 cleanup
  sysbench .* (re)
  \s* (re)
  Dropping table 'sbtest1'...
  Dropping table 'sbtest2'...

########################################################################
# Test --auto-inc=off
########################################################################
  $ sysbench ${OLTP_SCRIPT_PATH} ${DB_DRIVER_ARGS} --tables=1 --table-size=1000 --auto-inc=off --verbosity=1 prepare
  Creating table 'sbtest1'...
  Inserting 1000 records into 'sbtest1'
  Creating a secondary index on 'sbtest1'...

  $ sysbench ${OLTP_SCRIPT_PATH} ${DB_DRIVER_ARGS} --tables=1 --table-size=1000 --auto-inc=off --verbosity=1 --events=10 run

  $ sysbench ${OLTP_SCRIPT_PATH} ${DB_DRIVER_ARGS} --tables=1 --auto-inc=off --verbosity=1 cleanup
  Dropping table 'sbtest1'...

########################################################################
# Test --db-ps-mode=disable
########################################################################
  $ sysbench ${OLTP_SCRIPT_PATH} ${DB_DRIVER_ARGS} --tables=1 --table-size=1000 --db-ps-mode=disable --verbosity=1 prepare
  Creating table 'sbtest1'...
  Inserting 1000 records into 'sbtest1'
  Creating a secondary index on 'sbtest1'...

  $ sysbench ${OLTP_SCRIPT_PATH} ${DB_DRIVER_ARGS} --tables=1 --table-size=1000 --db-ps-mode=disable --verbosity=1 --events=10 run

  $ sysbench ${OLTP_SCRIPT_PATH} ${DB_DRIVER_ARGS} --tables=1 --db-ps-mode=disable --verbosity=1 cleanup
  Dropping table 'sbtest1'...
