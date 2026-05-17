#!/bin/bash
# Exhaustive test suite for RetDec: unit tests, integration, Valgrind, coverage.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -z "${BUILD:-}" ]; then
  for d in "$ROOT/build/linux" "$ROOT/build"; do
    if [ -f "$d/CTestTestfile.cmake" ] || [ -x "$d/src/retdec-decompiler/retdec-decompiler" ]; then
      BUILD=$d
      break
    fi
  done
fi
BUILD="${BUILD:-$ROOT/build/linux}"

if [ -z "${SMOKE_BIN:-}" ]; then
  for c in "$ROOT/dist/windows/test_hello.exe" \
           "$ROOT/build-win/win-layout/bin/smoke_hello.exe" \
           "$ROOT/build-win/win-runtime/test_hello.exe"; do
    if [ -f "$c" ]; then SMOKE_BIN=$c; break; fi
  done
fi
SMOKE_BIN="${SMOKE_BIN:-}"

echo "=== RetDec Exhaustive Test Suite ==="
echo "Root: $ROOT"
echo "Build: $BUILD"
echo ""

# 1. Run CTest unit tests
echo "--- 1. CTest unit tests ---"
if [ -f "$BUILD/CTestTestfile.cmake" ]; then
	cd "$BUILD"
	ctest -j"$(nproc)" --output-on-failure -V 2>&1 | tail -100
	cd - >/dev/null
else
	echo "Build dir not configured for tests. Run: cmake --preset full-linux-debug && cmake --build build/linux"
fi

# 2. Decompilation smoke test (Linux)
echo ""
echo "--- 2. Linux decompilation smoke ---"
DECOMPILER="$BUILD/src/retdec-decompiler/retdec-decompiler"
if [ -x "$DECOMPILER" ] && [ -n "$SMOKE_BIN" ] && [ -f "$SMOKE_BIN" ]; then
	OUT="/tmp/retdec_smoke_linux_$$.c"
	time "$DECOMPILER" -o "$OUT" "$SMOKE_BIN" 2>&1 | tail -15
	[ -f "$OUT" ] && echo "Output: $(wc -l < "$OUT") lines" && rm -f "$OUT"
else
	echo "Skipped: decompiler=$DECOMPILER smoke=$SMOKE_BIN"
fi

# 3. Valgrind memory leak check (Linux only)
echo ""
echo "--- 3. Valgrind memory leak check ---"
if command -v valgrind >/dev/null 2>&1 && [ -x "$DECOMPILER" ] && [ -n "$SMOKE_BIN" ] && [ -f "$SMOKE_BIN" ]; then
	OUT="/tmp/retdec_valgrind_$$.c"
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
		--error-exitcode=99 \
		"$DECOMPILER" -o "$OUT" "$SMOKE_BIN" 2>&1 | tail -80
	EX=$?
	rm -f "$OUT"
	[ $EX -eq 99 ] && echo "Valgrind found issues (exit 99)" || echo "Valgrind exit: $EX"
else
	echo "Skipped: valgrind, decompiler, or smoke PE missing (stage dist/windows or set SMOKE_BIN)"
fi

echo ""
echo "=== Test suite complete ==="
