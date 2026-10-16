########################################################################
Firebird driver: transaction parameters (--firebird-*-tpb)
MON$ISOLATION_MODE: 1 = snapshot, 4 = read committed with read consistency
########################################################################

  $ . $SBTEST_INCDIR/firebird_common.sh
  $ set +e

  $ cat >$CRAMTMP/tpb.lua <<'EOF'
  > local TX = "MON$READ_ONLY, MON$ISOLATION_MODE FROM MON$TRANSACTIONS " ..
  >            "WHERE MON$TRANSACTION_ID = CURRENT_TRANSACTION"
  > local function show(what, rs)
  >   local r = rs:fetch_row()
  >   print(string.format("%s: read_only=%s isolation=%s", what, r[1], r[2]))
  > end
  > function event()
  >   local con = sysbench.sql.driver():connect()
  >   con:query("EXECUTE BLOCK AS BEGIN IF (EXISTS(SELECT 1 FROM RDB$RELATIONS WHERE RDB$RELATION_NAME='TPB')) THEN EXECUTE STATEMENT 'DROP TABLE TPB'; END")
  >   con:query("CREATE TABLE tpb(ro INT, iso INT)")
  >   local sel = con:prepare("SELECT " .. TX)
  >   show("select outside BEGIN", sel:execute())
  >   show("query() outside BEGIN", con:query("SELECT " .. TX))
  >   con:query("BEGIN")
  >   show("select after BEGIN", sel:execute())
  >   con:query("COMMIT")
  >   -- An INSERT outside BEGIN runs in the write transaction. Read the row
  >   -- in a new transaction: a snapshot read transaction would not see it.
  >   local ins = con:prepare("INSERT INTO tpb SELECT " .. TX)
  >   ins:execute()
  >   con:query("BEGIN")
  >   show("write outside BEGIN", con:query("SELECT ro, iso FROM tpb"))
  >   con:query("COMMIT")
  >   sel:close()
  >   ins:close()
  >   con:query("DROP TABLE tpb")
  > end
  > EOF

  $ sysbench $CRAMTMP/tpb.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS run
  select outside BEGIN: read_only=1 isolation=4
  query() outside BEGIN: read_only=1 isolation=4
  select after BEGIN: read_only=0 isolation=1
  write outside BEGIN: read_only=0 isolation=1

  $ sysbench $CRAMTMP/tpb.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-read-tpb=snapshot-rw --firebird-begin-tpb=snapshot-ro --firebird-write-tpb=rc-rw run
  select outside BEGIN: read_only=0 isolation=1
  query() outside BEGIN: read_only=0 isolation=1
  select after BEGIN: read_only=1 isolation=1
  write outside BEGIN: read_only=0 isolation=4

  $ sysbench $CRAMTMP/tpb.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-read-tpb=rc-rw --firebird-begin-tpb=rc-ro run
  select outside BEGIN: read_only=0 isolation=4
  query() outside BEGIN: read_only=0 isolation=4
  select after BEGIN: read_only=1 isolation=4
  write outside BEGIN: read_only=0 isolation=1

########################################################################
# --firebird-autocommit-select: each SELECT outside BEGIN commits its
# read transaction, so the next one gets a new transaction
########################################################################
  $ cat >$CRAMTMP/autocommit.lua <<'EOF'
  > function event()
  >   local con = sysbench.sql.driver():connect()
  >   local st = con:prepare("SELECT CURRENT_TRANSACTION FROM RDB$DATABASE")
  >   local t1 = st:execute():fetch_row()[1]
  >   local t2 = st:execute():fetch_row()[1]
  >   print("new transaction for the second SELECT: " .. tostring(t1 ~= t2))
  > end
  > EOF
  $ sysbench $CRAMTMP/autocommit.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS run
  new transaction for the second SELECT: false
  $ sysbench $CRAMTMP/autocommit.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-autocommit-select=on run
  new transaction for the second SELECT: true

########################################################################
# A change outside BEGIN is the only open transaction of the connection:
# a prepare and a SELECT leave no read transaction open. Then the garbage
# collector is not held back. --firebird-autocommit=server runs the change
# in a read committed transaction with isc_tpb_autocommit. A transaction
# keeps its MON$ snapshot after commit retaining, so there each statement
# gets a new transaction (--firebird-autocommit-restart=1).
########################################################################
  $ cat >$CRAMTMP/ac.lua <<'EOF'
  > local TX = "MON$ISOLATION_MODE, MON$AUTO_COMMIT, " ..
  >   "(SELECT COUNT(*) FROM MON$TRANSACTIONS " ..
  >   " WHERE MON$ATTACHMENT_ID = CURRENT_CONNECTION) " ..
  >   "FROM MON$TRANSACTIONS WHERE MON$TRANSACTION_ID = CURRENT_TRANSACTION"
  > function event()
  >   local con = sysbench.sql.driver():connect()
  >   con:query("EXECUTE BLOCK AS BEGIN IF (EXISTS(SELECT 1 FROM RDB$RELATIONS WHERE RDB$RELATION_NAME='AC')) THEN EXECUTE STATEMENT 'DROP TABLE AC'; END")
  >   con:query("CREATE TABLE ac(what VARCHAR(20), iso INT, ac INT, n INT)")
  >   local ins = con:prepare("INSERT INTO ac SELECT 'after prepare', " .. TX)
  >   ins:execute()
  >   local n = con:query("SELECT COUNT(*) FROM ac"):fetch_row()[1]
  >   print("rows a SELECT sees after the change: " .. n)
  >   con:query("INSERT INTO ac SELECT 'after select', " .. TX)
  >   con:query("BEGIN")
  >   local rs = con:query("SELECT what, iso, ac, n FROM ac ORDER BY what")
  >   for i = 1, rs.nrows do
  >     local r = rs:fetch_row()
  >     print(string.format("%s: isolation=%s autocommit=%s transactions=%s",
  >                         r[1], r[2], r[3], r[4]))
  >   end
  >   con:query("COMMIT")
  >   ins:close()
  >   con:query("DROP TABLE ac")
  > end
  > EOF
  $ sysbench $CRAMTMP/ac.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS run
  rows a SELECT sees after the change: 1
  after prepare: isolation=1 autocommit=0 transactions=1
  after select: isolation=1 autocommit=0 transactions=1
  $ sysbench $CRAMTMP/ac.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-autocommit=server --firebird-autocommit-restart=1 run
  rows a SELECT sees after the change: 1
  after prepare: isolation=4 autocommit=1 transactions=1
  after select: isolation=4 autocommit=1 transactions=1
  $ sysbench $CRAMTMP/ac.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --db-ps-mode=disable run
  rows a SELECT sees after the change: 1
  after prepare: isolation=1 autocommit=0 transactions=1
  after select: isolation=1 autocommit=0 transactions=1

########################################################################
# Invalid values
########################################################################
  $ sysbench $CRAMTMP/tpb.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-read-tpb=dirty run
  FATAL: Invalid value for --firebird-read-tpb: 'dirty'. Use snapshot-rw, snapshot-ro, rc-rw or rc-ro.
  FATAL: `thread_run' function failed: * (glob)
  [1]
  $ sysbench $CRAMTMP/tpb.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-write-tpb=rc-ro run
  FATAL: --firebird-write-tpb must allow writes: use snapshot-rw or rc-rw, not 'rc-ro'.
  FATAL: `thread_run' function failed: * (glob)
  [1]
  $ sysbench $CRAMTMP/tpb.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-autocommit=sometimes run
  FATAL: Invalid value for --firebird-autocommit: 'sometimes'. Use client or server.
  FATAL: `thread_run' function failed: * (glob)
  [1]
