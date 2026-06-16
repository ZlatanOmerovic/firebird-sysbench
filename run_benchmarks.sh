#!/bin/bash
set -e

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
EVENTS="${EVENTS:-10000}"

COMMON="--db-driver=$DB_DRIVER $DB_ARGS --tables=$TABLES --table-size=$TABLE_SIZE"
RUN_ARGS="--threads=$THREADS --time=$TIME"

RESULTS_FILE="benchmark_${DB_DRIVER}_$(date +%Y%m%d_%H%M%S).txt"

echo "========================================" | tee "$RESULTS_FILE"
echo "Sysbench Benchmark Suite" | tee -a "$RESULTS_FILE"
echo "Driver: $DB_DRIVER" | tee -a "$RESULTS_FILE"
echo "Tables: $TABLES x $TABLE_SIZE rows" | tee -a "$RESULTS_FILE"
echo "Threads: $THREADS, Time: ${TIME}s" | tee -a "$RESULTS_FILE"
echo "Script start: $(date)" | tee -a "$RESULTS_FILE"
echo "========================================" | tee -a "$RESULTS_FILE"

# Initial cleanup — usually has nothing to drop, but if libfbclient or the
# server is unreachable this surfaces the failure with a real message.
# `set -e` is on, so any non-"table doesn't exist" failure will abort.
INIT_OUT=$(mktemp)
if ! $SYSBENCH "$SCRIPTS/oltp_read_write.lua" $COMMON --tables=8 cleanup > "$INIT_OUT" 2>&1; then
  if grep -qE "Unknown table|does not exist|unsuccessful metadata" "$INIT_OUT"; then
    : # expected on a fresh run, ignore
  else
    echo "========================================"
    echo "FATAL: $DB_DRIVER setup failed. sysbench output:"
    echo "----------------------------------------"
    cat "$INIT_OUT"
    echo "========================================"
    rm -f "$INIT_OUT"
    exit 1
  fi
fi
rm -f "$INIT_OUT"

BENCHMARKS_START=$(date +%s)
echo "Benchmarks start: $(date)" | tee -a "$RESULTS_FILE"
echo "========================================" | tee -a "$RESULTS_FILE"

run_test() {
  local script="$1"
  local name=$(basename "$script" .lua)

  echo "" | tee -a "$RESULTS_FILE"
  echo "--- $name ---" | tee -a "$RESULTS_FILE"

  if [ "$name" = "bulk_insert" ]; then
    $SYSBENCH "$script" --db-driver=$DB_DRIVER $DB_ARGS --threads=$THREADS cleanup > /dev/null 2>&1 || true
    $SYSBENCH "$script" --db-driver=$DB_DRIVER $DB_ARGS --threads=$THREADS prepare > /dev/null
    $SYSBENCH "$script" --db-driver=$DB_DRIVER $DB_ARGS --threads=$THREADS --events=$EVENTS run 2>&1 | tee -a "$RESULTS_FILE"
  elif [ "$name" = "select_random_points" ] || [ "$name" = "select_random_ranges" ]; then
    $SYSBENCH "$script" --db-driver=$DB_DRIVER $DB_ARGS --tables=1 --table-size=$TABLE_SIZE cleanup > /dev/null 2>&1 || true
    $SYSBENCH "$script" --db-driver=$DB_DRIVER $DB_ARGS --tables=1 --table-size=$TABLE_SIZE prepare > /dev/null
    $SYSBENCH "$script" --db-driver=$DB_DRIVER $DB_ARGS --tables=1 --table-size=$TABLE_SIZE $RUN_ARGS run 2>&1 | tee -a "$RESULTS_FILE"
  else
    $SYSBENCH "$script" $COMMON cleanup > /dev/null 2>&1 || true
    $SYSBENCH "$script" $COMMON prepare > /dev/null
    $SYSBENCH "$script" $COMMON $RUN_ARGS run 2>&1 | tee -a "$RESULTS_FILE"
  fi
}

run_test "$SCRIPTS/oltp_point_select.lua"
run_test "$SCRIPTS/oltp_read_only.lua"
run_test "$SCRIPTS/oltp_read_write.lua"
run_test "$SCRIPTS/oltp_insert.lua"
run_test "$SCRIPTS/oltp_delete.lua"
run_test "$SCRIPTS/oltp_update_index.lua"
run_test "$SCRIPTS/oltp_update_non_index.lua"
run_test "$SCRIPTS/select_random_points.lua"
run_test "$SCRIPTS/select_random_ranges.lua"
run_test "$SCRIPTS/bulk_insert.lua"

$SYSBENCH "$SCRIPTS/oltp_read_write.lua" $COMMON --tables=8 cleanup > /dev/null 2>&1 || true

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
