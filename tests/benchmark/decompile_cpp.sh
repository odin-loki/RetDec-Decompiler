#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RETDEC="${RETDEC:-$REPO_ROOT/build/linux/src/retdec-decompiler/retdec-decompiler}"
OUT="/tmp/benchmark_out"
echo "Decompiling C++ O1 unstripped..."
start=$(date +%s%N)
"$RETDEC" --keep-unreachable-funcs "$OUT/benchmark_cpp_O1" -o "$OUT/benchmark_cpp_O1.c" 2>"$OUT/benchmark_cpp_O1.log"
end=$(date +%s%N)
ms=$(( (end-start)/1000000 ))
echo "cpp_O1_unstripped: ${ms} ms" >> "$OUT/timing.txt"
echo "Done: $(wc -c <"$OUT/benchmark_cpp_O1.c") bytes  (${ms} ms)"

echo "Decompiling C++ O1 stripped..."
start=$(date +%s%N)
"$RETDEC" --keep-unreachable-funcs "$OUT/benchmark_cpp_O1_stripped" -o "$OUT/benchmark_cpp_O1_stripped.c" 2>"$OUT/benchmark_cpp_O1_stripped.log" || true
end=$(date +%s%N)
ms=$(( (end-start)/1000000 ))
echo "cpp_O1_stripped: ${ms} ms" >> "$OUT/timing.txt"
echo "Done stripped: $(wc -c <"$OUT/benchmark_cpp_O1_stripped.c") bytes  (${ms} ms)"
