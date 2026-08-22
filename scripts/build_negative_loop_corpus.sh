#!/usr/bin/env bash
# Build loop-containing B8 negatives (gcc -O0).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/tests/algorithm_recovery/sources/negative_loops"
OUT="${ROOT}/tests/algorithm_recovery/negative_loop_corpus"
python3 "${ROOT}/scripts/generate_negative_loop_corpus.py"
mkdir -p "${OUT}"
python3 - "${SRC}" "${OUT}" <<'PY'
import json, subprocess, sys
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])
if subprocess.run(["which", "gcc"], capture_output=True).returncode != 0:
    raise SystemExit("need gcc")
out.mkdir(parents=True, exist_ok=True)
cc_ver = subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0].strip()
manifest = []
for cfile in sorted(src.glob("*.c")):
    name = cfile.stem + "-gcc-O0"
    path = out / name
    subprocess.run(["gcc", "-O0", "-std=c11", "-o", str(path), str(cfile)], check=True)
    actual = path if path.is_file() else Path(str(path) + ".exe")
    subprocess.run(["strip", str(actual)], check=False)
    manifest.append({
        "name": name,
        "source": f"negative_loops/{cfile.name}",
        "compiler": "gcc",
        "cc_version": cc_ver,
        "opt": "O0",
        "path": actual.name,
    })
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Built {len(manifest)} loop-negative binaries in {out}")
if len(manifest) < 80:
    raise SystemExit(f"{len(manifest)} < 80")
PY
