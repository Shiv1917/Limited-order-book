#!/usr/bin/env bash
# Reproducible benchmark run on Linux: performance governor, pinned isolated
# core, perf counters per stage. Run as a user allowed to use sudo/perf.
#   CPU=3 ./scripts/bench_linux.sh data/aapl.obm
# For best results boot with isolcpus=$CPU nohz_full=$CPU rcu_nocbs=$CPU.
set -euo pipefail
STREAM=${1:-}
CPU=${CPU:-2}
BUILD=${BUILD:-build}
OUT=${OUT:-results/linux}
mkdir -p "$OUT"
if command -v cpupower >/dev/null; then sudo cpupower frequency-set -g performance >/dev/null || true; fi
echo "governor: $(cat /sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
echo "isolated: $(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
SRC=(--synthetic 2000000)
[ -n "$STREAM" ] && SRC=(--stream "$STREAM")
taskset -c "$CPU" "$BUILD/ob_bench" "${SRC[@]}" --cpu "$CPU" --repeats 7 --out-dir "$OUT" | tee "$OUT/bench.md"
if command -v perf >/dev/null; then
  for v in baseline flat pool hash cache; do
    echo "== perf stat: $v" | tee -a "$OUT/perf_stat.txt"
    taskset -c "$CPU" perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-load-misses,LLC-load-misses \
      "$BUILD/ob_bench" "${SRC[@]}" --cpu "$CPU" --repeats 3 --only "$v" 2>&1 >/dev/null | tee -a "$OUT/perf_stat.txt"
  done
  # Flame graph of the tuned book (needs https://github.com/brendangregg/FlameGraph on PATH).
  taskset -c "$CPU" perf record -F 4999 -g -o "$OUT/perf.data" "$BUILD/ob_bench" "${SRC[@]}" --cpu "$CPU" --repeats 3 --only cache >/dev/null
  if command -v stackcollapse-perf.pl >/dev/null; then
    perf script -i "$OUT/perf.data" | stackcollapse-perf.pl | flamegraph.pl > "$OUT/flamegraph.svg"
    echo "wrote $OUT/flamegraph.svg"
  fi
fi
