#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---------------- Build knobs ----------------
WITH_GRPC="${WITH_GRPC:-OFF}"              # ON/OFF (controls CMake option MSIM_WITH_GRPC)
CONFIG="${CONFIG:-Release}"                # VS config
GENERATOR="${GENERATOR:-Visual Studio 17 2022}"
PLATFORM="${PLATFORM:-x64}"
JOBS="${JOBS:-16}"
BUILD_TESTS="${BUILD_TESTS:-OFF}"

# Default build dirs (separate so grpc/non-grpc don't fight the cache)
if [[ -z "${BUILD_DIR:-}" ]]; then
  if [[ "$WITH_GRPC" == "ON" ]]; then BUILD_DIR="$ROOT/build_grpc"; else BUILD_DIR="$ROOT/build"; fi
fi
BUILD_DIR="${BUILD_DIR}"

# ---------------- Bench knobs ----------------
MODE="${MODE:-no_log}"                     # no_log | binlog | lmdb | grpc | binlog_grpc | lmdb_grpc
SCENARIO="${SCENARIO:-}"
SYMBOLS="${SYMBOLS:-AAPL,MSFT,GOOG,AMZN,NVDA,TSLA}"
EVENTS="${EVENTS:-2000000}"
THREADS="${THREADS:-6}"
SIGMA="${SIGMA:-0.001}"
ARENA_BYTES="${ARENA_BYTES:-1048576}"
REPS="${REPS:-5}"
WARMUP_EVENTS="${WARMUP_EVENTS:-200000}"
GRPC_TARGET="${GRPC_TARGET:-127.0.0.1:50051}"

# Output
OUTDIR="${OUTDIR:-$ROOT/benchmarks/$(date +%Y%m%d_%H%M%S)}"
QUIET_AFFINITY="${QUIET_AFFINITY:-1}"      # filter [Affinity] lines in console/log parsing

mkdir -p "$BUILD_DIR" "$OUTDIR"

# grpc modes should force grpc build
case "$MODE" in
  grpc|binlog_grpc|lmdb_grpc) WITH_GRPC="ON" ;;
esac

# ---------------- Configure & build ----------------
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo "[bench] configuring into $BUILD_DIR (WITH_GRPC=$WITH_GRPC)"
  cmake -S "$ROOT" -B "$BUILD_DIR" -G "$GENERATOR" -A "$PLATFORM" \
    -DMSIM_WITH_GRPC="$WITH_GRPC" -DMSIM_BUILD_TESTS="$BUILD_TESTS"
else
  # if cache exists, still ensure option matches (cheap reconfigure)
  cmake -S "$ROOT" -B "$BUILD_DIR" -G "$GENERATOR" -A "$PLATFORM" \
    -DMSIM_WITH_GRPC="$WITH_GRPC" -DMSIM_BUILD_TESTS="$BUILD_TESTS" >/dev/null
fi

echo "[bench] building ($CONFIG)"
cmake --build "$BUILD_DIR" --config "$CONFIG" -j "$JOBS"

BIN="$BUILD_DIR/$CONFIG/market_sim.exe"
if [[ ! -f "$BIN" ]]; then
  echo "ERROR: market_sim not found at $BIN" >&2
  exit 1
fi

COLLECTOR="$BUILD_DIR/$CONFIG/collector_server.exe"
HAS_COLLECTOR=0
if [[ -f "$COLLECTOR" ]]; then HAS_COLLECTOR=1; fi

# ---------------- Helpers ----------------
strip_cr() { tr -d '\r'; }
filter_affinity() {
  if [[ "$QUIET_AFFINITY" == "1" ]]; then grep -v '^\[Affinity\]' || true; else cat; fi
}

extract_last_num() {
  # Fixed-substring match using awk index(); returns the last numeric field on the matching line.
  # $1: full text
  # $2: literal key substring, e.g. "Book ops/sec:"
  # $3: awk regex for number token (int or float)
  local txt="$1" key="$2" numre="$3"
  echo "$txt" | strip_cr | awk -v k="$key" -v re="$numre" '
    index($0, k) {
      for (i = NF; i >= 1; --i) {
        if ($i ~ re) { v = $i; break }
      }
    }
    END { if (v != "") print v; }
  ' | tail -n 1
}

extract_int() {
  extract_last_num "$1" "$2" '^[0-9]+$'
}

extract_float() {
  extract_last_num "$1" "$2" '^[0-9]+([.][0-9]+)?$'
}

start_collector() {
  local addr="$1"
  local log="$2"

  if [[ "$HAS_COLLECTOR" != "1" ]]; then
    echo "ERROR: collector_server.exe not built (need WITH_GRPC=ON build)" >&2
    return 1
  fi

  # Start collector in background
  "$COLLECTOR" "$addr" > "$log" 2>&1 &
  local pid=$!

  # Give it a moment to bind
  sleep 0.25
  echo "$pid"
}

stop_collector() {
  local pid="$1"
  # Try kill; fallback to taskkill (Windows)
  kill "$pid" >/dev/null 2>&1 || true
  sleep 0.1
  if ps -p "$pid" >/dev/null 2>&1; then
    taskkill.exe //PID "$pid" //T //F >/dev/null 2>&1 || true
  fi
  wait "$pid" >/dev/null 2>&1 || true
}

# ---------------- Scenario naming ----------------
sym_count="$(echo "$SYMBOLS" | awk -F',' '{print NF}')"
if [[ -z "$SCENARIO" ]]; then
  SCENARIO="${MODE}_t${THREADS}_s${sym_count}_e${EVENTS}"
fi

# log path per mode
LOG_PATH=""
ARGS=(--symbols "$SYMBOLS" --events "$EVENTS" --threads "$THREADS" --sigma "$SIGMA" --arena-bytes "$ARENA_BYTES")

case "$MODE" in
  no_log)
    ARGS+=(--no-log)
    ;;
  binlog)
    LOG_PATH="$OUTDIR/${SCENARIO}.bin"
    rm -f "$LOG_PATH" || true
    ARGS+=(--log "$LOG_PATH")
    ;;
  lmdb)
    LOG_PATH="$OUTDIR/${SCENARIO}.mdb"
    rm -rf "$LOG_PATH" || true
    ARGS+=(--log "$LOG_PATH")
    ;;
  grpc)
    ARGS+=(--no-log --grpc "$GRPC_TARGET")
    ;;
  binlog_grpc)
    LOG_PATH="$OUTDIR/${SCENARIO}.bin"
    rm -f "$LOG_PATH" || true
    ARGS+=(--log "$LOG_PATH" --grpc "$GRPC_TARGET")
    ;;
  lmdb_grpc)
    LOG_PATH="$OUTDIR/${SCENARIO}.mdb"
    rm -rf "$LOG_PATH" || true
    ARGS+=(--log "$LOG_PATH" --grpc "$GRPC_TARGET")
    ;;
  *)
    echo "ERROR: unknown MODE=$MODE" >&2
    exit 2
    ;;
esac

# CSV
CSV="$OUTDIR/results.csv"
if [[ ! -f "$CSV" ]]; then
  echo "ts,scenario,mode,threads,symbols,events,rep,throughput_ev_s,steps_per_s,book_ops_per_s,adds,cancels,trades,action_ratio,elapsed_max_ms,collector_ev_s,log_path,grpc_target,build_dir,with_grpc" > "$CSV"
fi

echo "[bench] BIN:      $BIN"
echo "[bench] MODE:     $MODE"
echo "[bench] SCENARIO: $SCENARIO"
echo "[bench] OUTDIR:   $OUTDIR"
echo

# Warmup
WARGS=(--symbols "$SYMBOLS" --events "$WARMUP_EVENTS" --threads "$THREADS" --sigma "$SIGMA" --arena-bytes "$ARENA_BYTES")
case "$MODE" in
  no_log|grpc) WARGS+=(--no-log) ;;
  binlog|binlog_grpc) WARGS+=(--log "$OUTDIR/_warmup.bin") ;;
  lmdb|lmdb_grpc) WARGS+=(--log "$OUTDIR/_warmup.mdb") ;;
esac
case "$MODE" in
  grpc|binlog_grpc|lmdb_grpc) WARGS+=(--grpc "$GRPC_TARGET") ;;
esac
"$BIN" "${WARGS[@]}" >/dev/null 2>&1 || true

best_t=0;   sum_t=0
best_steps=0; sum_steps=0
best_ops=0; sum_ops=0

for rep in $(seq 1 "$REPS"); do
  raw_log="$OUTDIR/${SCENARIO}_rep${rep}.log"
  collector_log="$OUTDIR/${SCENARIO}_rep${rep}_collector.log"
  collector_rate=""

  collector_pid=""
  if [[ "$MODE" == *grpc* || "$MODE" == "grpc" ]]; then
    collector_pid="$(start_collector "$GRPC_TARGET" "$collector_log")"
  fi

  OUT="$("$BIN" "${ARGS[@]}" 2>&1 | strip_cr)"
  echo "$OUT" | filter_affinity | tee "$raw_log" >/dev/null

  if [[ -n "$collector_pid" ]]; then
    stop_collector "$collector_pid"
    # collector prints: "Received X events at Y ev/s"
    collector_rate="$(cat "$collector_log" | strip_cr | sed -nE 's/.*Received[[:space:]]+[0-9]+[[:space:]]+events[[:space:]]+at[[:space:]]+([0-9]+(\.[0-9]+)?).*/\1/p' | tail -n 1)"
  fi

  throughput="$(extract_int "$OUT" "Throughput:")"
  steps_s="$(extract_int "$OUT" "Steps/sec:")"
  book_ops_s="$(extract_int "$OUT" "Book ops/sec:")"
  adds="$(extract_int "$OUT" "Adds:")"
  cancels="$(extract_int "$OUT" "Cancels:")"
  trades="$(extract_int "$OUT" "Trades:")"
  elapsed_max_ms="$(extract_float "$OUT" "Elapsed (max):")"

  # Defaults if fields missing
  throughput="${throughput:-0}"
  steps_s="${steps_s:-0}"
  book_ops_s="${book_ops_s:-0}"
  adds="${adds:-0}"
  cancels="${cancels:-0}"
  trades="${trades:-0}"
  elapsed_max_ms="${elapsed_max_ms:-0}"

  ops=$((adds + cancels + trades))
  action_ratio="$(awk -v o="$ops" -v e="$EVENTS" 'BEGIN{ if(e>0) printf "%.6f", o/e; else print "0.0"; }')"

  # If the simulator didn't print these (single-thread path), derive them.
  if [[ "${steps_s:-0}" == "0" ]]; then
    steps_s="$throughput"   # throughput is steps/sec
  fi

  if [[ "${book_ops_s:-0}" == "0" ]]; then
    # book_ops/sec ~= steps/sec * (ops/steps)
    book_ops_s="$(awk -v t="$throughput" -v o="$ops" -v e="$EVENTS" \
      'BEGIN{ if(e>0) printf "%d", (t*o)/e; else print 0; }')"
  fi


  ts="$(date +%Y-%m-%dT%H:%M:%S%z)"
  echo "$ts,$SCENARIO,$MODE,$THREADS,$sym_count,$EVENTS,$rep,$throughput,$steps_s,$book_ops_s,$adds,$cancels,$trades,$action_ratio,$elapsed_max_ms,${collector_rate:-},${LOG_PATH:-},${GRPC_TARGET:-},$BUILD_DIR,$WITH_GRPC" >> "$CSV"

  echo
  echo "[bench] rep $rep:"
  echo "  throughput:   $throughput ev/s"
  echo "  steps/sec:    $steps_s"
  echo "  book ops/sec: $book_ops_s"
  echo "  action ratio: $action_ratio"
  if [[ -n "${collector_rate:-}" ]]; then
    echo "  collector:    $collector_rate ev/s"
  fi
  echo

  sum_t=$((sum_t + throughput))
  sum_ops=$((sum_ops + book_ops_s))
  sum_steps=$((sum_steps + steps_s))
  if (( throughput > best_t )); then best_t=$throughput; fi
  if (( book_ops_s > best_ops )); then best_ops=$book_ops_s; fi
  if (( steps_s > best_steps )); then best_steps=$steps_s; fi
done

avg_t=$((sum_t / REPS))
avg_steps=$((sum_steps / REPS))
avg_ops=$((sum_ops / REPS))

echo "[bench] CSV: $CSV"
echo "[bench] BEST throughput:   $best_t ev/s"
echo "[bench] BEST steps/sec:    $best_steps"
echo "[bench] BEST book ops/sec: $best_ops"
echo "[bench] AVG  throughput:   $avg_t ev/s"
echo "[bench] AVG  steps/sec:    $avg_steps"
echo "[bench] AVG  book ops/sec: $avg_ops"