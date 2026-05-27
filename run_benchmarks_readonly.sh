#!/bin/bash
set -eu

SCRIPT_START=$(date +%s)

SYSBENCH="./src/sysbench"
SCRIPTS="./src/lua"

DB_DRIVER="${1:-firebird}"

case "$DB_DRIVER" in
  firebird)
    DB_ARGS="--firebird-db=${FIREBIRD_DB:-localhost:/tmp/sbtest.fdb} --firebird-user=${FIREBIRD_USER:-SYSDBA} --firebird-password=${FIREBIRD_PASSWORD:-masterkey}"
    ;;
  mysql)
    DB_ARGS="--mysql-user=${MYSQL_USER:-sbtest} --mysql-password=${MYSQL_PASSWORD:-sbtest} --mysql-db=${MYSQL_DB:-sbtest}"
    ;;
  pgsql)
    DB_ARGS="--pgsql-host=${PGSQL_HOST:-localhost} --pgsql-user=${PGSQL_USER:-sbtest} --pgsql-password=${PGSQL_PASSWORD:-sbtest} --pgsql-db=${PGSQL_DB:-sbtest}"
    ;;
  *)
    echo "Usage: $0 [firebird|mysql|pgsql]"
    exit 1
    ;;
esac

TABLES="${TABLES:-4}"
TABLE_SIZE="${TABLE_SIZE:-10000}"
THREADS="${THREADS:-4}"
TIME="${TIME:-10}"

COMMON="--db-driver=$DB_DRIVER $DB_ARGS --tables=$TABLES --table-size=$TABLE_SIZE"
RUN_ARGS="--threads=$THREADS --time=$TIME"

RESULTS_FILE="benchmark_safe_${DB_DRIVER}_$(date +%Y%m%d_%H%M%S).txt"

echo "========================================" | tee "$RESULTS_FILE"
echo "Sysbench Read/Safe Benchmark Suite" | tee -a "$RESULTS_FILE"
echo "Driver: $DB_DRIVER" | tee -a "$RESULTS_FILE"
echo "Tables: $TABLES x $TABLE_SIZE rows" | tee -a "$RESULTS_FILE"
echo "Threads: $THREADS, Time: ${TIME}s" | tee -a "$RESULTS_FILE"
echo "Script start: $(date)" | tee -a "$RESULTS_FILE"
echo "========================================" | tee -a "$RESULTS_FILE"

$SYSBENCH "$SCRIPTS/oltp_read_write.lua" $COMMON cleanup 2>/dev/null || true

echo "Preparing tables..." | tee -a "$RESULTS_FILE"
$SYSBENCH "$SCRIPTS/oltp_read_write.lua" $COMMON prepare 2>&1 | tee -a "$RESULTS_FILE"

BENCHMARKS_START=$(date +%s)
echo "" | tee -a "$RESULTS_FILE"
echo "Benchmarks start: $(date)" | tee -a "$RESULTS_FILE"
echo "========================================" | tee -a "$RESULTS_FILE"

for script in oltp_point_select oltp_read_only oltp_read_write oltp_update_index oltp_update_non_index; do
  echo "" | tee -a "$RESULTS_FILE"
  echo "--- $script ---" | tee -a "$RESULTS_FILE"
  $SYSBENCH "$SCRIPTS/${script}.lua" $COMMON $RUN_ARGS run 2>&1 | tee -a "$RESULTS_FILE"
done

$SYSBENCH "$SCRIPTS/oltp_read_write.lua" $COMMON cleanup 2>/dev/null || true

SCRIPT_END=$(date +%s)
TOTAL_ELAPSED=$((SCRIPT_END - SCRIPT_START))
BENCH_ELAPSED=$((SCRIPT_END - BENCHMARKS_START))
PREP_ELAPSED=$((BENCHMARKS_START - SCRIPT_START))

echo "" | tee -a "$RESULTS_FILE"
echo "========================================" | tee -a "$RESULTS_FILE"
echo "Script start:     $(date -d @$SCRIPT_START '+%Y-%m-%d %H:%M:%S')" | tee -a "$RESULTS_FILE"
echo "Benchmarks start: $(date -d @$BENCHMARKS_START '+%Y-%m-%d %H:%M:%S')" | tee -a "$RESULTS_FILE"
echo "Script end:       $(date -d @$SCRIPT_END '+%Y-%m-%d %H:%M:%S')" | tee -a "$RESULTS_FILE"
echo "Prep time:        ${PREP_ELAPSED}s" | tee -a "$RESULTS_FILE"
echo "Benchmark time:   ${BENCH_ELAPSED}s" | tee -a "$RESULTS_FILE"
echo "Total time:       ${TOTAL_ELAPSED}s" | tee -a "$RESULTS_FILE"
echo "Results saved to: $RESULTS_FILE" | tee -a "$RESULTS_FILE"
echo "========================================" | tee -a "$RESULTS_FILE"
