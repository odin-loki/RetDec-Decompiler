#!/usr/bin/env bash
# Run every mingw test/example/fuzz binary under wine64 (64-bit prefix; no wine32 needed).
#   tests/wine_run.sh [build/win64]
set -u
B=${1:-build/win64}
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-forge64} WINEARCH=win64 WINEDEBUG=${WINEDEBUG:--all}
W=$(command -v /usr/lib/wine/wine64 || command -v wine64 || command -v wine)
[ -d "$WINEPREFIX/drive_c" ] || timeout 300 "$W" wineboot -i >/dev/null 2>&1   # syswow64 rundll32 error is harmless
fail=0
for t in "$B"/test_*.exe "$B"/example_*.exe "$B"/fuzz_*.exe; do
  timeout 600 "$W" "$t" > "${t%.exe}.wine.log" 2>&1; rc=$?
  case $rc in 0) echo "PASS $(basename "$t")";; 77) echo "SKIP $(basename "$t")";; *) echo "FAIL $(basename "$t") rc=$rc"; fail=1;; esac
done
exit $fail
