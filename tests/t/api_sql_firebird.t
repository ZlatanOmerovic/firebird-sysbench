########################################################################
SQL Lua API + Firebird tests
########################################################################

  $ . ${SBTEST_INCDIR}/firebird_common.sh

  $ SB_ARGS="--verbosity=1 --events=1 $DB_DRIVER_ARGS $CRAMTMP/api_sql.lua"

########################################################################
# Basic driver info and bulk insert
########################################################################
  $ cat >$CRAMTMP/api_sql.lua <<'EOF'
  > function event()
  >   local drv = sysbench.sql.driver()
  >   local con = drv:connect()
  >   local inspect = require("inspect")
  >   print("drv:name() = " .. drv:name())
  >   print("SQL types:")
  >   print(inspect(sysbench.sql.type))
  >   print('--')
  >   print("SQL error codes:")
  >   print(inspect(sysbench.sql.error))
  >   print('--')
  >   local e, m = pcall(sysbench.sql.driver, "non-existing")
  >   print(m)
  > end
  > EOF
  $ sysbench $SB_ARGS run
  drv:name() = firebird
  SQL types:
  {
    BIGINT = 4,
    CHAR = 11,
    DATE = 8,
    DATETIME = 9,
    DOUBLE = 6,
    FLOAT = 5,
    INT = 3,
    NONE = 0,
    SMALLINT = 2,
    TIME = 7,
    TIMESTAMP = 10,
    TINYINT = 1,
    VARCHAR = 12
  }
  --
  SQL error codes:
  {
    FATAL = 2,
    IGNORABLE = 1,
    NONE = 0
  }
  --
  FATAL: invalid database driver name: 'non-existing'
  failed to initialize the DB driver

########################################################################
# Bulk insert and query_row
########################################################################
  $ cat >$CRAMTMP/api_sql.lua <<'EOF'
  > function event()
  >   local drv = sysbench.sql.driver()
  >   local con = drv:connect()
  >   con:query("EXECUTE BLOCK AS BEGIN IF (EXISTS(SELECT 1 FROM RDB$RELATIONS WHERE RDB$RELATION_NAME='T')) THEN EXECUTE STATEMENT 'DROP TABLE T'; END")
  >   con:query("CREATE TABLE t(a INT NOT NULL)")
  >   con:bulk_insert_init("INSERT INTO t VALUES")
  >   for i = 1,100 do
  >     con:bulk_insert_next(string.format("(%d)", i))
  >   end
  >   con:bulk_insert_done()
  >   print(con:query_row("SELECT COUNT(DISTINCT a) FROM t"))
  >   con:query("DROP TABLE t")
  > end
  > EOF
  $ sysbench $SB_ARGS run
  100

########################################################################
# SELECT with NULLs and fetch_row
########################################################################
  $ cat >$CRAMTMP/api_sql.lua <<'EOF'
  > function event()
  >   local drv = sysbench.sql.driver()
  >   local con = drv:connect()
  >   con:query("EXECUTE BLOCK AS BEGIN IF (EXISTS(SELECT 1 FROM RDB$RELATIONS WHERE RDB$RELATION_NAME='T2')) THEN EXECUTE STATEMENT 'DROP TABLE T2'; END")
  >   con:query("CREATE TABLE t2(a INT, b VARCHAR(120), c DOUBLE PRECISION)")
  >   con:query("INSERT INTO t2 VALUES (1, 'foo', 0.4)")
  >   con:query("INSERT INTO t2 VALUES (NULL, 'bar', 0.2)")
  >   con:query("INSERT INTO t2 VALUES (2, NULL, 0.3)")
  >   con:query("INSERT INTO t2 VALUES (NULL, NULL, 0.1)")
  >   local rs = con:query("SELECT * FROM t2 ORDER BY a NULLS LAST, b NULLS LAST, c")
  >   for i = 1, rs.nrows do
  >      local row = rs:fetch_row()
  >      print(string.format("%s %s %s", row[1], row[2], row[3]))
  >   end
  >   print('--')
  >   print(string.format("%s %s", con:query_row("SELECT b, a FROM t2 ORDER BY b NULLS LAST, a")))
  >   print('--')
  >   con:query("DROP TABLE t2")
  > end
  > EOF
  $ sysbench $SB_ARGS run
  1 foo 0.* (re)
  2 nil 0.* (re)
  nil bar 0.* (re)
  nil nil 0.* (re)
  --
  bar nil
  --

########################################################################
# Multiple connections
########################################################################
  $ cat >$CRAMTMP/api_sql.lua <<'EOF'
  > function thread_init()
  >   drv = sysbench.sql.driver()
  >   con = {}
  >   for i=1,5 do
  >     con[i] = drv:connect()
  >   end
  > end
  > function event()
  >   con[1]:query("EXECUTE BLOCK AS BEGIN IF (EXISTS(SELECT 1 FROM RDB$RELATIONS WHERE RDB$RELATION_NAME='T')) THEN EXECUTE STATEMENT 'DROP TABLE T'; END")
  >   con[2]:query("CREATE TABLE t(a INT NOT NULL)")
  >   for i=1,5 do
  >     con[i]:query("INSERT INTO t VALUES (" .. i .. ")")
  >   end
  >   rs = con[1]:query("SELECT * FROM t ORDER BY a")
  >   for i = 1, rs.nrows do
  >      print(string.format("%s", unpack(rs:fetch_row(), 1, rs.nfields)))
  >   end
  >   con[1]:query("DROP TABLE t")
  > end
  > EOF
  $ sysbench $SB_ARGS run
  1
  2
  3
  4
  5

########################################################################
# Incorrect bulk API usage
########################################################################
  $ cat >$CRAMTMP/api_sql.lua <<'EOF'
  > c = sysbench.sql.driver():connect()
  > c:query("EXECUTE BLOCK AS BEGIN IF (EXISTS(SELECT 1 FROM RDB$RELATIONS WHERE RDB$RELATION_NAME='T1')) THEN EXECUTE STATEMENT 'DROP TABLE T1'; END")
  > c:query("CREATE TABLE t1(a INT NOT NULL)")
  > c:bulk_insert_init("INSERT INTO t1 VALUES")
  > c:bulk_insert_next("(1)")
  > c:bulk_insert_done()
  > e,m = pcall(function () c:bulk_insert_next("(2)") end)
  > print(m)
  > c:bulk_insert_done()
  > c:query("DROP TABLE t1")
  > EOF
  $ sysbench $SB_ARGS
  ALERT: attempt to call bulk_insert_next() before bulk_insert_init()
  */api_sql.lua:*: db_bulk_insert_next() failed (glob)

########################################################################
# query_row() with an empty result set
########################################################################
  $ cat >$CRAMTMP/api_sql.lua <<'EOF'
  > c = sysbench.sql.driver():connect()
  > c:query("EXECUTE BLOCK AS BEGIN IF (EXISTS(SELECT 1 FROM RDB$RELATIONS WHERE RDB$RELATION_NAME='T1')) THEN EXECUTE STATEMENT 'DROP TABLE T1'; END")
  > c:query("CREATE TABLE t1(a INT)")
  > print(c:query_row("SELECT * FROM t1"))
  > c:query("DROP TABLE t1")
  > EOF
  $ sysbench $SB_ARGS
  nil

########################################################################
# SELECT UNION (fetch_row iteration)
########################################################################
  $ cat >$CRAMTMP/api_sql.lua <<'EOF'
  > connection = sysbench.sql.driver():connect()
  > rows = connection:query("SELECT 1 FROM RDB$DATABASE UNION ALL SELECT 2 FROM RDB$DATABASE")
  > r = rows:fetch_row()
  > while ( r ) do
  >   print( r[ 1 ] )
  >   r = rows:fetch_row()
  > end
  > EOF
  $ sysbench $SB_ARGS
  1
  2

########################################################################
# Reconnect
########################################################################
  $ cat >$CRAMTMP/api_sql.lua <<'EOF'
  > function sysbench.hooks.report_cumulative(stat)
  >   print("reconnects = " .. stat.reconnects)
  > end
  > function thread_init()
  >   drv = sysbench.sql.driver()
  >   con = drv:connect()
  > end
  > function event()
  >   print(con:query_row("SELECT 1 FROM RDB$DATABASE"))
  >   con:reconnect()
  >   print(con:query_row("SELECT 2 FROM RDB$DATABASE"))
  >   print('--')
  > end
  > EOF
  $ sysbench $SB_ARGS run
  1
  2
  --
  reconnects = 1

########################################################################
# Failed connection handling
########################################################################
  $ cat >$CRAMTMP/api_sql.lua <<'EOF'
  > function event()
  >   local drv = sysbench.sql.driver()
  >   local e,m = pcall(drv.connect, drv)
  >   print(m)
  > end
  > EOF
  $ sysbench $SB_ARGS --firebird-db="non-existing:/tmp/noexist.fdb" run
  FATAL: isc_attach_database() failed:
  FATAL: * (glob)
  FATAL: * (glob)
  FATAL: * (glob)
  connection creation failed
