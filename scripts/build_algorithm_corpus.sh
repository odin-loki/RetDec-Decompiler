#!/usr/bin/env bash
# build_algorithm_corpus.sh — Build algorithm-recovery corpus (step 10).
# Usage: bash scripts/build_algorithm_corpus.sh [--out DIR]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/tests/algorithm_recovery/sources"
OUT="${ROOT}/tests/algorithm_recovery/corpus"
OPTS=(O0 O2 O3)

while [[ $# -gt 0 ]]; do
	case "$1" in
		--out) OUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

python3 - "${SRC}" "${OUT}" "${OPTS[*]}" <<'PY'
import json, subprocess, sys
from pathlib import Path

src_dir, out_dir, opts_s = sys.argv[1:4]
src = Path(src_dir)
out = Path(out_dir)
opts = opts_s.split()
compilers = []
for cc in ("gcc", "clang"):
    if subprocess.run(["which", cc], capture_output=True).returncode == 0:
        compilers.append(cc)
if not compilers:
    raise SystemExit("Need gcc or clang")

out.mkdir(parents=True, exist_ok=True)
manifest = []
for cfile in sorted(src.glob("*.c")):
    base = cfile.stem
    for cc in compilers:
        for opt in opts:
            name = f"{base}-{cc}-{opt}"
            path = out / name
            subprocess.run([cc, f"-{opt}", "-o", str(path), str(cfile)], check=True)
            subprocess.run(["strip", str(path)], check=False)
            manifest.append({
                "name": name,
                "source": cfile.name,
                "compiler": cc,
                "opt": opt,
                "path": str(path),
            })

(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Built {len(manifest)} binaries in {out}")
PY

python3 "${ROOT}/scripts/generate_ground_truth.py" \
	--sources "${SRC}" \
	--manifest "${OUT}/manifest.json" \
	--out "${ROOT}/tests/algorithm_recovery/ground_truth/corpus.json"
