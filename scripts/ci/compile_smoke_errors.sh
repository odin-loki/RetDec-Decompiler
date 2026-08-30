#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ART="${ROOT}/tests/decompilebench/artifacts/fork"
echo "=== gcc -fsyntax-only first artifact ==="
f="$(ls "${ART}"/*.c | head -n1)"
echo "FILE $f"
gcc -fsyntax-only -std=c11 -w "$f" 2>&1 | head -n 40 || true
echo "=== buildable ==="
b="${f%.c}.buildable.c"
if [[ ! -f "$b" ]]; then
	stem="${f%.c}"
	b="$(dirname "$f")/$(basename "${stem}").buildable.c"
	# also try sibling without -O0
	b2="$(ls "${ART}"/*.buildable.c 2>/dev/null | head -n1 || true)"
	[[ -n "${b2}" ]] && b="$b2"
fi
echo "FILE $b"
if [[ -f "$b" ]]; then
	gcc -fsyntax-only -std=c11 -w "$b" 2>&1 | head -n 50 || true
fi
echo "=== json summary ==="
python3 - <<PY
import json
p=json.load(open("${ROOT}/results/decompilebench-ci-core.json",encoding="utf-8"))
print(json.dumps(p.get("summary"), indent=2))
print("COMPARE", json.dumps(p.get("compare"), indent=2))
print("walls", [s.get("wall_s") for s in p.get("samples",[])])
print("tu", [s.get("tu_valid") for s in p.get("samples",[])])
print("buildable", [s.get("tu_valid_buildable") for s in p.get("samples",[])])
PY
