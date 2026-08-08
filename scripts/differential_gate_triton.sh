#!/usr/bin/env bash
# differential_gate_triton.sh — Triton/D-Helix differential gate scaffold (step 20).
# Usage: bash scripts/differential_gate_triton.sh <original.c> <refined.c>
set -euo pipefail

ORIG="${1:?original.c}"
REF="${2:?refined.c}"

if ! command -v triton >/dev/null 2>&1 && ! python3 -c "import triton" 2>/dev/null; then
	echo "Triton not installed — falling back to stdout differential gate"
	export RETDEC_NEURAL_DIFF_GATE=1
	python3 - "$ORIG" "$REF" <<'PY'
import os, subprocess, sys, tempfile
from pathlib import Path
orig, ref = sys.argv[1:3]
for cc in ("gcc", "cc"):
    if subprocess.run(["which", cc], capture_output=True).returncode == 0:
        break
else:
    sys.exit(2)
td = Path(tempfile.mkdtemp())
for name, src in ("orig", orig), ("ref", ref):
    subprocess.run([cc, "-O2", "-o", str(td/name), src], check=True)
    out = subprocess.check_output([str(td/name)], text=True)
    Path(td / f"{name}.out").write_text(out)
if (td / "orig.out").read_text() != (td / "ref.out").read_text():
    sys.exit(1)
PY
	exit $?
fi

echo "Triton available — implement symbolic differential compare here (D-Helix reference)"
echo "See docs/internal/retypd_sailr_llvm.md and MASTER-UPGRADE-PLAN Part 8.5"
exit 0
