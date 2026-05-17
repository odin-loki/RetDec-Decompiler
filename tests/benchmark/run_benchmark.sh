#!/usr/bin/env bash
# run_benchmark.sh — Compile benchmark.c at O1/O2 (stripped & unstripped)
# then decompile all four variants and record wall-clock time.
#
# Usage: bash run_benchmark.sh [retdec-build-dir]
#   Default build dir: <repo>/build/linux (repo = parent of tests/)
#
# Outputs (in /tmp/benchmark_out/):
#   benchmark_O1[_stripped].c  benchmark_O2[_stripped].c
#   timing.txt                 (decompiler wall-clock times)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC="$SCRIPT_DIR/benchmark.c"

BUILD_DIR="${1:-$REPO_ROOT/build/linux}"
RETDEC="$BUILD_DIR/src/retdec-decompiler/retdec-decompiler"

OUT_DIR="/tmp/benchmark_out"
mkdir -p "$OUT_DIR"

TIMING_FILE="$OUT_DIR/timing.txt"
> "$TIMING_FILE"

# ─── helpers ──────────────────────────────────────────────────────────────────
log()  { echo "[run_benchmark] $*"; }
die()  { echo "[run_benchmark] ERROR: $*" >&2; exit 1; }

check_tool() {
    command -v "$1" >/dev/null 2>&1 || die "Tool not found: $1"
}

# ─── prerequisite checks ──────────────────────────────────────────────────────
check_tool gcc
check_tool strip
check_tool time
[ -x "$RETDEC" ] || die "retdec-decompiler not found at $RETDEC"

log "Source   : $SRC"
log "RetDec   : $RETDEC"
log "Output   : $OUT_DIR"
echo ""

# ─── compile ──────────────────────────────────────────────────────────────────
compile() {
    local level=$1 extra_flags=$2 out_name=$3
    log "Compiling ($out_name)..."
    gcc "$level" $extra_flags -o "$OUT_DIR/$out_name" "$SRC"
    log "  -> $OUT_DIR/$out_name  ($(wc -c <"$OUT_DIR/$out_name") bytes)"
}

# -mno-sse/-mno-sse2 is required at -O2: GCC may emit SSE instructions that crash
# retdec's capstone lifter (translateSseMovLane0 assertion).
compile -O1 ""                       "benchmark_O1"
compile -O1 "-s"                     "benchmark_O1_stripped"
compile -O2 "-mno-sse -mno-sse2"     "benchmark_O2"
compile -O2 "-mno-sse -mno-sse2 -s"  "benchmark_O2_stripped"

# C++ variants
SRC_CPP="$SCRIPT_DIR/benchmark_cpp.cpp"
compile_cpp() {
    local level=$1 extra_flags=$2 out_name=$3
    log "Compiling C++ ($out_name)..."
    g++ "$level" -std=c++17 $extra_flags -o "$OUT_DIR/$out_name" "$SRC_CPP"
    log "  -> $OUT_DIR/$out_name  ($(wc -c <"$OUT_DIR/$out_name") bytes)"
}

compile_cpp -O1 ""   "benchmark_cpp_O1"
compile_cpp -O1 "-s" "benchmark_cpp_O1_stripped"
echo ""

# ─── decompile ────────────────────────────────────────────────────────────────
decompile() {
    local binary=$1 label=$2
    local bin_path="$OUT_DIR/$binary"
    local out_base="$OUT_DIR/$binary"

    log "Decompiling $binary ..."
    local start end elapsed
    start=$(date +%s%N)
    "$RETDEC" --keep-unreachable-funcs "$bin_path" \
        -o "${out_base}.c" \
        2>"${out_base}.log" || {
        log "  WARNING: decompiler returned non-zero (see ${out_base}.log)"
    }
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))   # ms
    log "  -> ${out_base}.c  (${elapsed} ms)"
    echo "$label: ${elapsed} ms" >> "$TIMING_FILE"
}

decompile "benchmark_O1"          "O1_unstripped"
decompile "benchmark_O1_stripped" "O1_stripped"
decompile "benchmark_O2"          "O2_unstripped"
decompile "benchmark_O2_stripped" "O2_stripped"
decompile "benchmark_cpp_O1"           "cpp_O1_unstripped"
decompile "benchmark_cpp_O1_stripped"  "cpp_O1_stripped"
echo ""

# ─── summary ──────────────────────────────────────────────────────────────────
log "=== Decompiler Timing ==="
cat "$TIMING_FILE"
echo ""
log "All outputs are in $OUT_DIR"
log "Next step: bash $SCRIPT_DIR/score_benchmark.sh"
