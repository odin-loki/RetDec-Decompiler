#!/bin/bash
# Run RetDec decompiler built with AddressSanitizer + LeakSanitizer.
# Build: cmake --preset core-asan (or -DRETDEC_USE_ADDRESS_SANITIZER=ON) then build retdec-decompiler
# Usage: wsl bash scripts/run_asan.sh [binary_to_decompile]
set -e
cd "$(dirname "$0")/.."
DECOMPILER=""
for d in build/linux build; do
  if [ -f "$d/src/retdec-decompiler/retdec-decompiler" ]; then
    DECOMPILER="$d/src/retdec-decompiler/retdec-decompiler"
    break
  fi
done
TEST_BIN="${1:-}"
if [ -z "$TEST_BIN" ]; then
  for tb in dist/windows/test_hello.exe build-win/win-runtime/test_hello.exe; do
    if [ -f "$tb" ]; then TEST_BIN=$tb; break; fi
  done
fi
OUTPUT="profile_output/asan_out.c"
[ -n "$DECOMPILER" ] || { echo "Decompiler not found under build/linux or build/. Build with ASan (e.g. core-asan preset)."; exit 1; }
[ -n "$TEST_BIN" ] || { echo "Test binary not found (pass path or stage dist/windows/test_hello.exe)"; exit 1; }
mkdir -p "$(dirname "$OUTPUT")"
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:symbolize=1"
export LSAN_OPTIONS="report_objects=1"
echo "Running $DECOMPILER with ASan+LSan on $TEST_BIN"
echo "Output: $OUTPUT"
"$DECOMPILER" "$TEST_BIN" -o "$OUTPUT" --silent 2>&1 | tee profile_output/asan.log
