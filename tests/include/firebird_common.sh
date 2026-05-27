########################################################################
# Common code for Firebird-specific tests
########################################################################
set -eu

if [ -z "${SBTEST_FIREBIRD_ARGS:-}" ]
then
  exit 80
fi

SBTEST_FB_DB=$(echo "$SBTEST_FIREBIRD_ARGS" | sed -n 's/.*--firebird-db=\([^ ]*\).*/\1/p')
SBTEST_FB_USER=$(echo "$SBTEST_FIREBIRD_ARGS" | sed -n 's/.*--firebird-user=\([^ ]*\).*/\1/p')
SBTEST_FB_PASS=$(echo "$SBTEST_FIREBIRD_ARGS" | sed -n 's/.*--firebird-password=\([^ ]*\).*/\1/p')

SBTEST_FB_ISQL="${SBTEST_FB_BINDIR:-$(command -v isql 2>/dev/null || command -v isql-fb 2>/dev/null || echo isql)}"

function db_show_table() {
  $SBTEST_FB_ISQL -user "$SBTEST_FB_USER" -password "$SBTEST_FB_PASS" \
    "$SBTEST_FB_DB" -q <<EOF | sed '/^$/d'
SET HEADING ON;
SELECT rf.RDB\$FIELD_NAME AS "Column",
       CASE f.RDB\$FIELD_TYPE
         WHEN 7 THEN 'SMALLINT'
         WHEN 8 THEN 'INTEGER'
         WHEN 16 THEN 'BIGINT'
         WHEN 14 THEN 'CHAR(' || f.RDB\$FIELD_LENGTH || ')'
         WHEN 37 THEN 'VARCHAR(' || f.RDB\$FIELD_LENGTH || ')'
         WHEN 27 THEN 'DOUBLE PRECISION'
         WHEN 10 THEN 'FLOAT'
         ELSE 'OTHER'
       END AS "Type",
       IIF(rf.RDB\$NULL_FLAG = 1, 'NOT NULL', 'NULLABLE') AS "Null"
FROM RDB\$RELATION_FIELDS rf
JOIN RDB\$FIELDS f ON rf.RDB\$FIELD_SOURCE = f.RDB\$FIELD_NAME
WHERE rf.RDB\$RELATION_NAME = UPPER('$1')
ORDER BY rf.RDB\$FIELD_POSITION;
SHOW INDEX $1;
EOF
  if [ $? -ne 0 ]; then
    echo "Table '$1' does not exist."
  fi
}

DB_DRIVER_ARGS="--db-driver=firebird $SBTEST_FIREBIRD_ARGS"
