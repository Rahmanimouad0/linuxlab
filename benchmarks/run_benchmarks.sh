#!/usr/bin/env bash
# Run every benchmark, store the raw CSVs and a description of the machine
# in results/, then draw SVG charts from the CSVs.
#
#   benchmarks/run_benchmarks.sh [bin-dir]      (default: build/bin)
#
# Tips for stable numbers: close other programs, plug in the laptop, and run
# twice - the medians of two runs should agree within a few percent.
set -eu
cd "$(dirname "$0")/.."
BIN=${1:-build/bin}
OUT=${OUT:-results}
mkdir -p "$OUT"
export LINUXLAB_BENCH_DIR=${LINUXLAB_BENCH_DIR:-$OUT}

{
    echo "date:      $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "kernel:    $(uname -srm)"
    echo "cpu:       $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "cpus:      $(nproc) online"
    echo "memory:    $(awk '/MemTotal/ {printf "%.1f GiB", $2 / 1048576}' /proc/meminfo)"
    echo "compiler:  $(${CC:-cc} --version | head -n 1)"
    echo "file dir:  $LINUXLAB_BENCH_DIR ($(df -T "$LINUXLAB_BENCH_DIR" | awk 'NR == 2 {print $2}'))"
    if grep -qi microsoft /proc/version; then
        echo "note:      WSL2 - Linux runs in a Hyper-V virtual machine"
    fi
} > "$OUT/environment.txt"
cat "$OUT/environment.txt"

for b in spawn ipc sync fileio; do
    echo ">> bench_$b"
    "$BIN/bench_$b" --csv | tee "$OUT/$b.csv"
done

python3 benchmarks/plot.py "$OUT" || echo "(python3 not available: charts skipped)"
