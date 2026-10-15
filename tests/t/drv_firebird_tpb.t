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
