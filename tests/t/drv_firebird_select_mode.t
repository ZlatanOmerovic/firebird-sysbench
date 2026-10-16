########################################################################
Firebird driver: --firebird-select-mode
########################################################################

  $ . $SBTEST_INCDIR/firebird_common.sh
  $ set +e

  $ cat >$CRAMTMP/select_mode.lua <<'EOF'
  > function event()
  >   local con = sysbench.sql.driver():connect()
  >   con:query("EXECUTE BLOCK AS BEGIN IF (EXISTS(SELECT 1 FROM RDB$RELATIONS WHERE RDB$RELATION_NAME='T')) THEN EXECUTE STATEMENT 'DROP TABLE T'; END")
  >   con:query("CREATE TABLE t(a INT NOT NULL PRIMARY KEY)")
  >   for i = 1, 5 do
  >     con:query("INSERT INTO t VALUES (" .. i .. ")")
  >   end
  >   local st = con:prepare("SELECT a FROM t WHERE a BETWEEN ? AND ? ORDER BY a")
  >   local p1 = st:bind_create(sysbench.sql.type.INT)
  >   local p2 = st:bind_create(sysbench.sql.type.INT)
  >   st:bind_param(p1, p2)
  >   local function run(a, b)
  >     p1:set(a)
  >     p2:set(b)
  >     local rs = st:execute()
  >     local rows = {}
  >     for i = 1, rs.nrows do
  >       rows[i] = rs:fetch_row()[1]
  >     end
  >     print(string.format("%d..%d: %d rows [%s]", a, b, rs.nrows,
  >                         table.concat(rows, ",")))
  >   end
  >   run(2, 2)
  >   run(10, 20)
  >   run(1, 5)
  >   run(3, 4)
  >   st:close()
  >   con:query("DROP TABLE t")
  > end
  > EOF

########################################################################
# singleton: one row and no row work, more than one row is fatal
########################################################################
  $ sysbench $CRAMTMP/select_mode.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-select-mode=singleton run
  2..2: 1 rows [2]
  10..20: 0 rows []
  FATAL: execute() failed: SQLCODE -811 multiple rows in singleton select
  FATAL: failed query was: SELECT a FROM t WHERE a BETWEEN ? AND ? ORDER BY a
  FATAL: The SELECT returned more than one row. --firebird-select-mode=singleton allows one row only; use --firebird-select-mode=auto or cursor.
  FATAL: `thread_run' function failed: * (glob)
  [1]

########################################################################
# auto: falls back to the cursor after the first result with more rows;
# cursor: the same results
########################################################################
  $ for mode in auto cursor
  > do
  >   sysbench $CRAMTMP/select_mode.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-select-mode=$mode run
  > done
  2..2: 1 rows [2]
  10..20: 0 rows []
  1..5: 5 rows [1,2,3,4,5]
  3..4: 2 rows [3,4]
  2..2: 1 rows [2]
  10..20: 0 rows []
  1..5: 5 rows [1,2,3,4,5]
  3..4: 2 rows [3,4]

########################################################################
# Invalid value
########################################################################
  $ sysbench $CRAMTMP/select_mode.lua --verbosity=1 --events=1 $DB_DRIVER_ARGS --firebird-select-mode=fast run
  FATAL: Invalid value for --firebird-select-mode: 'fast'. Use auto, singleton or cursor.
  FATAL: `thread_run' function failed: * (glob)
  [1]
