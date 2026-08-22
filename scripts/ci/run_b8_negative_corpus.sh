#!/usr/bin/env bash
# Decompile the B8 negative corpus and count detector false positives.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
CORPUS="${ROOT}/tests/algorithm_recovery/negative_corpus"
if [[ ! -x "${DEC}" ]]; then
	echo "missing ${DEC}" >&2
	exit 1
fi
if [[ ! -f "${CORPUS}/manifest.json" ]]; then
	bash "${ROOT}/scripts/build_negative_corpus.sh"
fi
DEC_DIR="$(cd "$(dirname "${DEC}")" && pwd)"
SHARE_DIR="$(cd "${DEC_DIR}/.." && pwd)/share/retdec"
mkdir -p "${SHARE_DIR}/profiles"
cp -f "${ROOT}/src/retdec-decompiler/decompiler-config.json" "${SHARE_DIR}/"
python3 "${ROOT}/scripts/extract_decompiler_predictions.py" \
	--decompiler "${DEC}" \
	--corpus "${CORPUS}" \
	--manifest "${CORPUS}/manifest.json" \
	--out "${ROOT}/results/b8-negative-predictions.json" \
	--work "${ROOT}/build/linux/b8-work" \
	--jobs "$(nproc)" \
	--timeout 120
python3 - "${ROOT}" <<'PY'
import json
from pathlib import Path
root = Path(__import__("sys").argv[1])
payload = json.loads((root / "results/b8-negative-predictions.json").read_text(encoding="utf-8"))
preds = payload.get("predictions_raw") or payload.get("predictions") or {}
pos = {k: v for k, v in preds.items() if v}
n = payload.get("requested") or len(preds)
ok = payload.get("decompiled") or 0
fp = len(pos)
rate = (fp / n) if n else 0.0
md = root / "results/b8-negative-corpus.md"
lines = [
    "# B8 negative corpus",
    "",
    "200+ binaries whose sources are not target algorithm-recovery labels",
    "(unit conversion, clamp, lerp, date, BMI, PID, flags, IPv4, mortgage stub,",
    "log level). Ground truth is empty. Any extracted label is a false positive.",
    "",
    f"- binaries requested: {n}",
    f"- decompiled: {ok}",
    f"- binaries with any label: **{fp}**",
    f"- false-positive rate: **{rate:.3f}**",
    "",
]
if pos:
    lines.append("## Positives (false positives)")
    lines.append("")
    for name in sorted(pos):
        labels = ", ".join(pos[name])
        lines.append(f"- `{name}`: {labels}")
    lines.append("")
else:
    lines.append("No labels extracted (0 false positives on this run).")
    lines.append("")
md.write_text("\n".join(lines), encoding="utf-8")
summary = {
    "requested": n,
    "decompiled": ok,
    "false_positive_binaries": fp,
    "false_positive_rate": rate,
    "positives": pos,
}
(root / "results/b8-negative-corpus.json").write_text(
    json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
print(f"B8_OK requested={n} decompiled={ok} fp={fp} rate={rate:.3f}")
PY
