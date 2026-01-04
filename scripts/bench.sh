#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
CONFIG="${CONFIG:-Release}"          # Release / Debug for VS generator
JOBS="${JOBS:-16}"

# Benchmark parameters (override via env if you want)
SYMBOLS="${SYMBOLS:-AAPL,MSFT,GOOG,AMZN,NVDA,TSLA}"
EVENTS="${EVENTS:-2000000}"
THREADS="${THREADS:-6}"
SIGMA="${SIGMA:-0.001}"
ARENA_BYTES="${ARENA_BYTES:-1048576}"
REPS="${REPS:-5}"

mkdir -p "$BUILD"

# Configure once (or reconfigure if cache missing)
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
  echo "[bench] configuring into $BUILD"
  cmake -S "$ROOT" -B "$BUILD" -G "Visual Studio 17 2022" -A x64 -DMSIM_WITH_GRPC=OFF -DMSIM_BUILD_TESTS=ON
else
  echo "[bench] using existing build cache in $BUILD"
fi

echo "[bench] building ($CONFIG)"
cmake --build "$BUILD" --config "$CONFIG" -j "$JOBS"

# Locate binary (VS generator puts it under build/<config>/)
BIN="$BUILD/$CONFIG/market_sim.exe"
if [[ ! -f "$BIN" ]]; then
  # Fallback for single-config generators (Ninja, etc.)
  BIN="$BUILD/market_sim"
fi
if [[ ! -f "$BIN" ]]; then
  echo "ERROR: market_sim binary not found in $BUILD"
  exit 1
fi

echo "[bench] binary: $BIN"
echo "[bench] running: symbols=$SYMBOLS events=$EVENTS threads=$THREADS sigma=$SIGMA arena=$ARENA_BYTES reps=$REPS"
echo

# Warmup (don’t record)
"$BIN" --symbols "$SYMBOLS" --events 200000 --threads "$THREADS" --sigma "$SIGMA" --arena-bytes "$ARENA_BYTES" --no-log >/dev/null

best=0
sum=0
count=0

for i in $(seq 1 "$REPS"); do
  out="$("$BIN" --symbols "$SYMBOLS" --events "$EVENTS" --threads "$THREADS" --sigma "$SIGMA" --arena-bytes "$ARENA_BYTES" --no-log)"
  echo "$out"

  # Parse: line like "Throughput:        25700000 ev/s"
  tput="$(echo "$out" | grep -i "Throughput:" | awk '{print $2}' | tr -d '\r')"
  if [[ -z "$tput" ]]; then
    echo "ERROR: could not parse throughput"
    exit 1
  fi

  sum=$((sum + tput))
  count=$((count + 1))
  if (( tput > best )); then best=$tput; fi
  echo "[bench] run $i throughput: $tput ev/s"
  echo
done

avg=$((sum / count))
echo "[bench] BEST: $best ev/s"
echo "[bench] AVG : $avg ev/s"
