#!/bin/bash
# Run Valgrind on Linux decompiler. Use from WSL: wsl bash scripts/run_valgrind.sh
# Output is saved to profile_output/valgrind.log (or path given as $1)
set -e
cd "$(dirname "$0")/.."
OUTPUT="${1:-profile_output/valgrind.log}"
mkdir -p "$(dirname "$OUTPUT")"

BUILD=""
for d in build/linux build build-wsl; do
  if [ -f "$d/src/retdec-decompiler/retdec-decompiler" ]; then
    BUILD=$d
    break
  fi
done
[ -n "$BUILD" ] || { echo "No decompiler binary under build/linux, build/, or build-wsl/"; exit 1; }
DECOMPILER="$BUILD/src/retdec-decompiler/retdec-decompiler"

TEST_BIN=""
for tb in dist/windows/test_hello.exe build-win/win-runtime/test_hello.exe; do
  if [ -f "$tb" ]; then TEST_BIN=$tb; break; fi
done
[ -n "$TEST_BIN" ] || { echo "No test binary (try dist/windows/test_hello.exe or build-win/win-runtime/test_hello.exe)"; exit 1; }

echo "Running Valgrind on $DECOMPILER with $TEST_BIN"
echo "Output will be saved to $OUTPUT"
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
  "$DECOMPILER" "$TEST_BIN" -o /tmp/valgrind_out.c --silent 2>&1 | tee "$OUTPUT"
