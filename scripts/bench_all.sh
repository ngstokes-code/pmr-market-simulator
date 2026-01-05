#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OUTDIR="${OUTDIR:-$ROOT/benchmarks/full_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUTDIR"
echo "[bench_all] out dir: $OUTDIR"

# Shared knobs (override via env)
REPS="${REPS:-5}"
SIGMA="${SIGMA:-0.001}"
ARENA_BYTES="${ARENA_BYTES:-1048576}"

THREADS_MT="${THREADS_MT:-6}"
SYMBOLS_MT="${SYMBOLS_MT:-AAPL,MSFT,GOOG,AMZN,NVDA,TSLA}"
EVENTS_MT="${EVENTS_MT:-2000000}"

THREADS_ST="${THREADS_ST:-1}"
SYMBOLS_ST="${SYMBOLS_ST:-AAPL}"
EVENTS_ST="${EVENTS_ST:-2000000}"

# gRPC
RUN_GRPC="${RUN_GRPC:-1}"
GRPC_TARGET="${GRPC_TARGET:-127.0.0.1:50051}"

# Optional: quiet affinity lines (bench.sh already supports this)
QUIET_AFFINITY="${QUIET_AFFINITY:-1}"

echo "[bench_all] --- baseline no-log (multi-thread) ---"
OUTDIR="$OUTDIR" MODE="no_log" SCENARIO="baseline_mt_nolog" WITH_GRPC="OFF" \
  THREADS="$THREADS_MT" SYMBOLS="$SYMBOLS_MT" EVENTS="$EVENTS_MT" \
  SIGMA="$SIGMA" ARENA_BYTES="$ARENA_BYTES" REPS="$REPS" QUIET_AFFINITY="$QUIET_AFFINITY" \
  ./scripts/bench.sh

echo "[bench_all] --- baseline no-log (single-thread) ---"
OUTDIR="$OUTDIR" MODE="no_log" SCENARIO="baseline_st_nolog" WITH_GRPC="OFF" \
  THREADS="$THREADS_ST" SYMBOLS="$SYMBOLS_ST" EVENTS="$EVENTS_ST" \
  SIGMA="$SIGMA" ARENA_BYTES="$ARENA_BYTES" REPS="$REPS" QUIET_AFFINITY="$QUIET_AFFINITY" \
  ./scripts/bench.sh

echo "[bench_all] --- binary logging (single-thread) ---"
OUTDIR="$OUTDIR" MODE="binlog" SCENARIO="binlog_st" WITH_GRPC="OFF" \
  THREADS="$THREADS_ST" SYMBOLS="$SYMBOLS_ST" EVENTS="$EVENTS_ST" \
  SIGMA="$SIGMA" ARENA_BYTES="$ARENA_BYTES" REPS="$REPS" QUIET_AFFINITY="$QUIET_AFFINITY" \
  ./scripts/bench.sh

echo "[bench_all] --- LMDB logging (single-thread) ---"
OUTDIR="$OUTDIR" MODE="lmdb" SCENARIO="lmdb_st" WITH_GRPC="OFF" \
  THREADS="$THREADS_ST" SYMBOLS="$SYMBOLS_ST" EVENTS="$EVENTS_ST" \
  SIGMA="$SIGMA" ARENA_BYTES="$ARENA_BYTES" REPS="$REPS" QUIET_AFFINITY="$QUIET_AFFINITY" \
  ./scripts/bench.sh

if [[ "$RUN_GRPC" == "1" ]]; then
  echo "[bench_all] --- gRPC streaming (multi-thread, no-log) ---"
  set +e
  OUTDIR="$OUTDIR" MODE="grpc" SCENARIO="grpc_mt_nolog" WITH_GRPC="ON" \
    THREADS="$THREADS_MT" SYMBOLS="$SYMBOLS_MT" EVENTS="$EVENTS_MT" GRPC_TARGET="$GRPC_TARGET" \
    SIGMA="$SIGMA" ARENA_BYTES="$ARENA_BYTES" REPS="$REPS" QUIET_AFFINITY="$QUIET_AFFINITY" \
    ./scripts/bench.sh
  rc=$?
  set -e
  if [[ $rc -ne 0 ]]; then
    echo "[bench_all] WARN: gRPC scenario failed. Is a collector port free / grpc build enabled? Skipping gRPC cases." >&2
    RUN_GRPC=0
  fi
fi

if [[ "$RUN_GRPC" == "1" ]]; then
  echo "[bench_all] --- gRPC + binlog (single-thread) ---"
  OUTDIR="$OUTDIR" MODE="binlog_grpc" SCENARIO="binlog_grpc_st" WITH_GRPC="ON" \
    THREADS="$THREADS_ST" SYMBOLS="$SYMBOLS_ST" EVENTS="$EVENTS_ST" GRPC_TARGET="$GRPC_TARGET" \
    SIGMA="$SIGMA" ARENA_BYTES="$ARENA_BYTES" REPS="$REPS" QUIET_AFFINITY="$QUIET_AFFINITY" \
    ./scripts/bench.sh

  echo "[bench_all] --- gRPC + LMDB (single-thread) ---"
  OUTDIR="$OUTDIR" MODE="lmdb_grpc" SCENARIO="lmdb_grpc_st" WITH_GRPC="ON" \
    THREADS="$THREADS_ST" SYMBOLS="$SYMBOLS_ST" EVENTS="$EVENTS_ST" GRPC_TARGET="$GRPC_TARGET" \
    SIGMA="$SIGMA" ARENA_BYTES="$ARENA_BYTES" REPS="$REPS" QUIET_AFFINITY="$QUIET_AFFINITY" \
    ./scripts/bench.sh
fi

echo "[bench_all] done"
echo "[bench_all] results: $OUTDIR/results.csv"
