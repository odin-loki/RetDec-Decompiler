#!/usr/bin/env bash
# Build the B9 adversarial-positive corpus (gcc -O0 and -O2).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/tests/algorithm_recovery/sources/adversarial"
OUT="${ROOT}/tests/algorithm_recovery/adversarial_corpus"
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
    extra = ["-std=c11"]
    if cfile.stem == "aes_ni":
        extra += ["-maes", "-msse2"]
    for opt in ("O0", "O2"):
        name = f"{cfile.stem}-gcc-{opt}"
        path = out / name
        cmd = ["gcc", f"-{opt}", *extra, "-o", str(path), str(cfile)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"SKIP {name}: {proc.stderr.strip().splitlines()[-1] if proc.stderr else 'compile failed'}")
            continue
        actual = path if path.is_file() else Path(str(path) + ".exe")
        subprocess.run(["strip", str(actual)], check=False)
        manifest.append({
            "name": name,
            "source": f"adversarial/{cfile.name}",
            "compiler": "gcc",
            "cc_version": cc_ver,
            "opt": opt,
            "path": actual.name,
        })
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Built {len(manifest)} adversarial binaries in {out}")
if len(manifest) < 8:
    raise SystemExit(f"{len(manifest)} < 8")
PY
python3 "${ROOT}/scripts/generate_ground_truth.py" \
	--sources "${SRC}" \
	--manifest "${OUT}/manifest.json" \
	--out "${ROOT}/tests/algorithm_recovery/ground_truth/adversarial.json"
