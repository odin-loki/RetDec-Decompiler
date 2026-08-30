#!/usr/bin/env bash
set -euo pipefail
ART=/mnt/c/Users/odinl/OneDrive/Desktop/RetDec/tests/decompilebench/artifacts/fork
for f in "$ART"/*-gcc-O0-O0.buildable.c; do
	[[ -f "$f" ]] || continue
	echo "===== $(basename "$f") ====="
	gcc -fsyntax-only -std=gnu11 -w "$f" 2>&1 | head -n 8 || true
done
