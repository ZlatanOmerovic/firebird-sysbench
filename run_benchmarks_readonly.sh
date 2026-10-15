#!/bin/bash
set -eu

SCRIPT_START=$(date +%s)

SYSBENCH="./src/sysbench"
SCRIPTS="./src/lua"

DB_DRIVER="${1:-firebird}"

# Connection arguments (TRANSPORT), warm-up and the results header.
. "$(dirname "$0")/run_benchmarks_common.sh"

TABLES="${TABLES:-4}"
TABLE_SIZE="${TABLE_SIZE:-10000}"
THREADS="${THREADS:-4}"
TIME="${TIME:-10}"

COMMON="--db-driver=$DB_DRIVER $DB_ARGS --tables=$TABLES --table-size=$TABLE_SIZE"
RUN_ARGS="--threads=$THREADS --time=$TIME --warmup-time=$WARMUP"

RESULTS_FILE="benchmark_safe_${DB_DRIVER}_$(date +%Y%m%d_%H%M%S).txt"

echo "========================================" | tee "$RESULTS_FILE"
echo "Sysbench Read/Safe Benchmark Suite" | tee -a "$RESULTS_FILE"
echo "Driver: $DB_DRIVER" | tee -a "$RESULTS_FILE"
echo "Connection: $(echo "$DB_ARGS" | sed -E 's/(-password=)[^ ]*/\1***/g')" | tee -a "$RESULTS_FILE"
if [ "$DB_DRIVER" = firebird ]; then
  echo "Client library: $FB_CLIENT" | tee -a "$RESULTS_FILE"
fi
echo "Tables: $TABLES x $TABLE_SIZE rows" | tee -a "$RESULTS_FILE"
echo "Threads: $THREADS, Time: ${TIME}s" | tee -a "$RESULTS_FILE"
echo "Script start: $(date)" | tee -a "$RESULTS_FILE"
echo "========================================" | tee -a "$RESULTS_FILE"

$SYSBENCH "$SCRIPTS/oltp_read_write.lua" $COMMON cleanup 2>/dev/null || true

echo "Preparing tables..." | tee -a "$RESULTS_FILE"
$SYSBENCH "$SCRIPTS/oltp_read_write.lua" $COMMON --threads=$PREPARE_THREADS prepare 2>&1 | tee -a "$RESULTS_FILE"

firebird_set_linger
write_settings_header

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
