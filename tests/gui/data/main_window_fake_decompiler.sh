#!/bin/sh
# A stand-in for retdec-decompiler (tests/gui/main_window_test.cpp): reads
# "<binary> -o <out.c> ...", logs like the real one, then writes the sidecars
# of tests/gui/data/main_window/fixture.* next to <out.c>.
#   FAKE_DECOMPILER_EXIT=N   exit with N (no output written when N != 0)
#   FAKE_DECOMPILER_SLEEP=S  sleep S seconds before finishing (stop / cancel tests)
#   FAKE_DECOMPILER_ARGS=F   append the argument vector to file F
#   FAKE_DECOMPILER_FIXTURE=D the fixture directory (default: main_window/ beside this script)
here=$(cd "$(dirname "$0")" && pwd)
fixture="${FAKE_DECOMPILER_FIXTURE:-$here/main_window}"
out=""
prev=""
for a in "$@"; do
	if [ "$prev" = "-o" ]; then out="$a"; fi
	prev="$a"
done
if [ -n "$FAKE_DECOMPILER_ARGS" ]; then printf '%s\n' "$@" >> "$FAKE_DECOMPILER_ARGS"; fi
echo "Running phase: Initialization ( 0.01s )"
echo "Running phase: Input binary to LLVM IR decoding ( 0.02s )"
echo " -> stack ( 0.03s )"
echo "Warning: unresolved indirect jump at 0x401020"
echo " -> simple types ( 0.04s )"
echo "[progress] code generation 50%"
if [ -n "$FAKE_DECOMPILER_SLEEP" ]; then sleep "$FAKE_DECOMPILER_SLEEP"; fi
code="${FAKE_DECOMPILER_EXIT:-0}"
if [ "$code" != "0" ]; then
	echo "Error: fake decompiler failure"
	exit "$code"
fi
if [ -z "$out" ]; then
	echo "Error: no -o given"
	exit 2
fi
base="${out%.c}"
cp "$fixture/fixture.c" "$out" || exit 3
cp "$fixture/fixture.config.json" "$base.config.json" || exit 3
cp "$fixture/fixture.dsm" "$base.dsm" || exit 3
cp "$fixture/fixture.ll" "$base.ll" || exit 3
echo "Done."
exit 0
