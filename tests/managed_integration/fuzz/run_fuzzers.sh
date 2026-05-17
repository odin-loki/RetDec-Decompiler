#!/usr/bin/env bash
# run_fuzzers.sh — Run all managed-language parser fuzz targets.
#
# Usage (from repo root after building with RETDEC_FUZZ=ON):
#   bash tests/managed_integration/fuzz/run_fuzzers.sh [--runs N] [--jobs N]
#
# Options:
#   --runs N     Total fuzzer executions per target (default: 1000000)
#   --jobs N     Parallel fuzzer processes per target (default: CPU count)
#   --build DIR  Build directory containing fuzzer binaries (default: build/linux)
#
# The fuzzer will populate per-target corpus directories and write crash
# artifacts next to each binary as crash-XXXXXXXX files.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
FUZZ_SRC="$REPO_ROOT/tests/managed_integration/fuzz"
FIXTURES="$REPO_ROOT/tests/managed_integration/fixtures"
BUILD_DIR="$REPO_ROOT/build/linux"
RUNS=1000000
JOBS="$(nproc)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --runs)  RUNS="$2";  shift 2;;
        --jobs)  JOBS="$2";  shift 2;;
        --build) BUILD_DIR="$2"; shift 2;;
        *) echo "Unknown: $1" >&2; exit 1;;
    esac
done

FUZZ_BIN_DIR="$BUILD_DIR/tests/managed_integration/fuzz"

# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------
run_fuzzer() {
    local name="$1"
    local corpus="$2"
    local seed_dir="$3"
    local binary="$FUZZ_BIN_DIR/$name"

    if [[ ! -x "$binary" ]]; then
        echo "[skip] $name not built (did you cmake -DRETDEC_FUZZ=ON?)"
        return
    fi

    mkdir -p "$corpus"

    # Seed corpus from fixtures if seed dir exists
    if [[ -d "$seed_dir" ]]; then
        find "$seed_dir" -maxdepth 1 -type f | while read -r f; do
            cp -n "$f" "$corpus/" 2>/dev/null || true
        done
    fi

    echo "=== Fuzzing $name (runs=$RUNS, jobs=$JOBS) ==="
    "$binary" "$corpus" \
        -runs="$RUNS" \
        -jobs="$JOBS" \
        -workers="$JOBS" \
        -max_len=65536 \
        -print_final_stats=1 \
        -artifact_prefix="$FUZZ_BIN_DIR/${name}_crash_" \
    2>&1 | tee "$FUZZ_BIN_DIR/${name}.fuzz.log" || true

    # Count crashes
    crashes=$(find "$FUZZ_BIN_DIR" -maxdepth 1 -name "${name}_crash_*" | wc -l)
    echo "  Crashes found: $crashes"
    if [[ $crashes -gt 0 ]]; then
        echo "  Crash files:"
        ls "$FUZZ_BIN_DIR/${name}_crash_"* 2>/dev/null | head -5
    fi
    echo ""
}

# ---------------------------------------------------------------------------
# Run each fuzzer
# ---------------------------------------------------------------------------
mkdir -p "$FUZZ_BIN_DIR"

run_fuzzer fuzz_jvm_class \
    "$FUZZ_BIN_DIR/corpus_jvm" \
    "$FIXTURES/java"

run_fuzzer fuzz_dex \
    "$FUZZ_BIN_DIR/corpus_dex" \
    "$FIXTURES/dex"

run_fuzzer fuzz_wasm \
    "$FUZZ_BIN_DIR/corpus_wasm" \
    "$FIXTURES/wasm"

run_fuzzer fuzz_pyc \
    "$FUZZ_BIN_DIR/corpus_pyc" \
    "$FIXTURES/python"

echo "=== All fuzz runs complete ==="
echo "Logs:    $FUZZ_BIN_DIR/*.fuzz.log"
echo "Corpus:  $FUZZ_BIN_DIR/corpus_*"
echo "Crashes: $FUZZ_BIN_DIR/*_crash_*"
