#!/usr/bin/env bash
# run_full_benchmark.sh — One-shot: compile, decompile, score, compare.
#
# Usage: bash run_full_benchmark.sh [retdec-build-dir] [--save-baseline]
#
#   --save-baseline   copies the new score.json to baseline.json in this dir

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="/tmp/benchmark_out"

SAVE_BASELINE=0
BUILD_DIR=""
for arg in "$@"; do
    if [ "$arg" = "--save-baseline" ]; then
        SAVE_BASELINE=1
    elif [ -z "$BUILD_DIR" ] && [ "${arg#-}" = "$arg" ]; then
        BUILD_DIR="$arg"
    fi
done
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/linux}"

echo "=== Step 1/3: compile & decompile ==="
bash "$SCRIPT_DIR/run_benchmark.sh" "$BUILD_DIR"

echo ""
echo "=== Step 2/3: score ==="
bash "$SCRIPT_DIR/score_benchmark.sh" "$OUT_DIR" "$OUT_DIR/score.json"

if [ "$SAVE_BASELINE" -eq 1 ]; then
    cp "$OUT_DIR/score.json" "$SCRIPT_DIR/baseline.json"
    echo "Baseline saved to $SCRIPT_DIR/baseline.json"
fi

echo ""
echo "=== Step 3/3: compare against baseline ==="
if [ -f "$SCRIPT_DIR/baseline.json" ]; then
    bash "$SCRIPT_DIR/compare_benchmark.sh" \
         "$OUT_DIR/score.json" "$SCRIPT_DIR/baseline.json"
else
    echo "(No baseline.json found — skipping comparison.)"
    echo "Run with --save-baseline to establish a baseline."
fi
