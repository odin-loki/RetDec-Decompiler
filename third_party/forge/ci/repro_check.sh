#!/bin/sh
# Reproducible-build check: two clean Release builds in different directories must produce bit-identical
# forge, forge-serve and libforge_c. Usage: ci/repro_check.sh [cmake args...]
set -eu
src=$(cd "$(dirname "$0")/.." && pwd)
a=$(mktemp -d /tmp/forge-repro-a.XXXX); b=$(mktemp -d /tmp/forge-repro-bb.XXXX)
for d in "$a" "$b"; do
  cmake -S "$src" -B "$d" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFORGE_BUILD_TESTS=OFF "$@" >/dev/null
  ninja -C "$d" forge forge-serve forge_c >/dev/null
done
rc=0
for f in forge forge-serve libforge.so; do
  ha=$(sha256sum "$a/$f" | cut -d' ' -f1); hb=$(sha256sum "$b/$f" | cut -d' ' -f1)
  if [ "$ha" = "$hb" ]; then echo "identical  $f  $ha"; else echo "DIFFERENT  $f  $ha  $hb"; rc=1; fi
done
if [ $rc = 0 ]; then rm -rf "$a" "$b"; else echo "kept $a $b"; fi
exit $rc
