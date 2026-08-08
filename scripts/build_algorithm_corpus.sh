#!/usr/bin/env bash
# build_algorithm_corpus.sh — Build algorithm-recovery corpus (step 10, 200+ binaries).
# Usage: bash scripts/build_algorithm_corpus.sh [--out DIR] [--no-generate]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/tests/algorithm_recovery/sources"
OUT="${ROOT}/tests/algorithm_recovery/corpus"
OPTS=(O0 O2 O3)
GENERATE=1

while [[ $# -gt 0 ]]; do
	case "$1" in
		--out) OUT="$2"; shift 2 ;;
		--no-generate) GENERATE=0; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ "${GENERATE}" -eq 1 ]]; then
	python3 "${ROOT}/scripts/generate_corpus_sources.py"
fi

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
for cfile in sorted(src.rglob("*.c")):
    rel = cfile.relative_to(src)
    source_key = str(rel).replace("\\", "/")
    stem = source_key.replace("/", "_").removesuffix(".c")
    text = cfile.read_text(encoding="utf-8", errors="replace")
    extra = ["-std=c11"]
    link = []
    if "pthread" in text:
        link.append("-pthread")
    for cc in compilers:
        for opt in opts:
            name = f"{stem}-{cc}-{opt}"
            path = out / name
            cmd = [cc, f"-{opt}", *extra, "-o", str(path), str(cfile), *link]
            subprocess.run(cmd, check=True)
            subprocess.run(["strip", str(path)], check=False)
            manifest.append({
                "name": name,
                "source": source_key,
                "compiler": cc,
                "opt": opt,
                "path": str(path),
            })

(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Built {len(manifest)} binaries in {out}")
if len(manifest) < 200:
    print(f"WARNING: {len(manifest)} < 200 binaries — add more sources to catalog", file=sys.stderr)
PY

python3 "${ROOT}/scripts/generate_ground_truth.py" \
	--sources "${SRC}" \
	--manifest "${OUT}/manifest.json" \
	--out "${ROOT}/tests/algorithm_recovery/ground_truth/corpus.json"
