#!/bin/bash
# [BiSheng] AIMV benchmark runner (T4.6)
# Usage: ./run_bench.sh [benchmark_name]
# Runs all benchmarks if no name given. Outputs timing data to results.json.
set -e

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS="${BENCH_DIR}/results.json"
CC="${CC:-clang}"
CFLAGS="-O2 -g"
WARMUP=3
RUNS=10

echo "[" > "$RESULTS"

first=true
for bench in dep_fail_alias dep_fail_stride cost_reject align_unknown multi_fail; do
  if [ $# -gt 0 ] && [ "$1" != "$bench" ]; then
    continue
  fi
  src="${BENCH_DIR}/${bench}.c"
  bin="${BENCH_DIR}/${bench}.out"

  echo "  Compiling $bench..."
  $CC $CFLAGS "$src" -o "$bin" 2>&1 || { echo "  SKIP: $bench (compile failed)"; continue; }

  echo "  Warming up $bench ($WARMUP runs)..."
  for i in $(seq 1 $WARMUP); do "$bin" > /dev/null 2>&1; done

  echo "  Measuring $bench ($RUNS runs)..."
  times=()
  for i in $(seq 1 $RUNS); do
    start=$(date +%s%N)
    "$bin" > /dev/null 2>&1
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))  # ms
    times+=($elapsed)
  done

  # Compute median
  IFS=$'\n' sorted=($(sort -n <<<"${times[*]}"))
  unset IFS
  mid=$((RUNS / 2))
  median=${sorted[$mid]}

  $first || echo "  ," >> "$RESULTS"
  first=false
  echo -n "  {\"benchmark\": \"$bench\", \"median_ms\": $median, \"runs\": [$(
    IFS=,
    echo "${times[*]}"
    unset IFS
  )]}" >> "$RESULTS"

  rm -f "$bin"
done

echo "" >> "$RESULTS"
echo "]" >> "$RESULTS"
echo "Results written to $RESULTS"
