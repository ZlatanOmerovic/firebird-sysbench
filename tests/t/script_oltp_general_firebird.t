########################################################################
General OLTP Firebird tests (table structure verification)
########################################################################

  $ . $SBTEST_INCDIR/firebird_common.sh
  $ ARGS="oltp_read_write ${DB_DRIVER_ARGS} --verbosity=1"
  $ FB_ISQL_CMD="FIREBIRD=$(dirname $(dirname $SBTEST_FB_ISQL)) LD_LIBRARY_PATH=$(dirname $(dirname $SBTEST_FB_ISQL))/lib $SBTEST_FB_ISQL -user $SBTEST_FB_USER -password $SBTEST_FB_PASS $SBTEST_FB_DB -q"

########################################################################
# Verify table structure after prepare
########################################################################
  $ sysbench $ARGS prepare
  Creating table 'sbtest1'...
  Inserting 10000 records into 'sbtest1'
  Creating a secondary index on 'sbtest1'...

  $ eval $FB_ISQL_CMD <<'SQL' | grep -E 'INTEGER|CHAR'
  > SELECT TRIM(rf.RDB$FIELD_NAME) AS COL, CASE f.RDB$FIELD_TYPE WHEN 8 THEN 'INTEGER' WHEN 14 THEN 'CHAR' ELSE 'OTHER' END AS FTYPE, IIF(rf.RDB$NULL_FLAG = 1, 'NOT NULL', 'NULLABLE') AS NULLABLE FROM RDB$RELATION_FIELDS rf JOIN RDB$FIELDS f ON rf.RDB$FIELD_SOURCE = f.RDB$FIELD_NAME WHERE rf.RDB$RELATION_NAME = 'SBTEST1' ORDER BY rf.RDB$FIELD_POSITION;
  > QUIT;
  > SQL
  ID.*INTEGER.*NOT NULL.* (re)
  K.*INTEGER.*NOT NULL.* (re)
  C.*CHAR.*NOT NULL.* (re)
  PAD.*CHAR.*NOT NULL.* (re)

  $ eval $FB_ISQL_CMD <<'SQL' | grep INDEX
  > SHOW INDEX SBTEST1;
  > QUIT;
  > SQL
  K_1 INDEX ON SBTEST1.* (re)
  RDB\$PRIMARY.* UNIQUE INDEX ON SBTEST1\(ID\).* (re)

  $ sysbench $ARGS cleanup
  Dropping table 'sbtest1'...

########################################################################
# Test --create-secondary=off
########################################################################
  $ sysbench $ARGS --create-secondary=off prepare
  Creating table 'sbtest1'...
  Inserting 10000 records into 'sbtest1'

  $ eval $FB_ISQL_CMD <<'SQL' | grep INDEX
  > SHOW INDEX SBTEST1;
  > QUIT;
  > SQL
  RDB\$PRIMARY.* UNIQUE INDEX ON SBTEST1\(ID\).* (re)

  $ sysbench $ARGS cleanup
  Dropping table 'sbtest1'...

########################################################################
# Test --auto-inc=off
########################################################################
  $ sysbench $ARGS --auto-inc=off prepare
  Creating table 'sbtest1'...
  Inserting 10000 records into 'sbtest1'
  Creating a secondary index on 'sbtest1'...

  $ sysbench $ARGS --auto-inc=off --events=10 run

  $ sysbench $ARGS --auto-inc=off cleanup
  Dropping table 'sbtest1'...
