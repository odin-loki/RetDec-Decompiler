#!/usr/bin/env bash
# Decompile loop-containing negatives and publish FP + confidence observations (A4).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
CORPUS="${ROOT}/tests/algorithm_recovery/negative_loop_corpus"
if [[ ! -x "${DEC}" ]]; then
	echo "missing ${DEC}" >&2
	exit 1
fi
if [[ ! -f "${CORPUS}/manifest.json" ]]; then
	bash "${ROOT}/scripts/build_negative_loop_corpus.sh"
fi
DEC_DIR="$(cd "$(dirname "${DEC}")" && pwd)"
SHARE_DIR="$(cd "${DEC_DIR}/.." && pwd)/share/retdec"
mkdir -p "${SHARE_DIR}/profiles"
cp -f "${ROOT}/src/retdec-decompiler/decompiler-config.json" "${SHARE_DIR}/"
python3 "${ROOT}/scripts/extract_decompiler_predictions.py" \
	--decompiler "${DEC}" \
	--corpus "${CORPUS}" \
	--manifest "${CORPUS}/manifest.json" \
	--out "${ROOT}/results/b8-loop-negative-predictions.json" \
	--work "${ROOT}/build/linux/b8-loop-work" \
	--jobs "$(nproc)" \
	--timeout 120 \
	--no-stem-fallback
python3 - "${ROOT}" <<'PY'
import json
from collections import defaultdict
from pathlib import Path

root = Path(__import__("sys").argv[1])
payload = json.loads((root / "results/b8-loop-negative-predictions.json").read_text(encoding="utf-8"))
preds = payload.get("predictions_raw") or payload.get("predictions") or {}
pos = {k: v for k, v in preds.items() if v}
n = payload.get("requested") or len(preds)
ok = payload.get("decompiled") or 0
fp = len(pos)
rate = (fp / n) if n else 0.0

work = root / "build/linux/b8-loop-work"
by_label = defaultdict(list)
for name in pos:
    cfg_path = work / name / f"{name}.config.json"
    if not cfg_path.is_file():
        cfg_path = work / name / f"{name}.c.config.json"
    if not cfg_path.is_file():
        continue
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    for fn in cfg.get("functions") or []:
        for det in fn.get("semanticDetections") or []:
            label = det.get("label") or ""
            kind = det.get("kind") or ""
            conf = float(det.get("confidence") or 0.0)
            if label:
                by_label[f"{kind}:{label}"].append(conf)

calib = []
for key, confs in sorted(by_label.items()):
    calib.append({
        "key": key,
        "n": len(confs),
        "mean_confidence": sum(confs) / len(confs),
        "min_confidence": min(confs),
        "max_confidence": max(confs),
    })

md = [
    "# B8 loop-containing negatives",
    "",
    "FIR / histogram / Bresenham / box-blur / HTTP-verb / UTF-8 scan /",
    "sliding-max / transpose / saturating-add / dot-product. Empty labels.",
    "These *do* have loops, so they stress `strlen` / `atoi` / `dfs` heuristics",
    "and the sort opcode bag. Any extracted label is a false positive.",
    "",
    f"- binaries requested: {n}",
    f"- decompiled: {ok}",
    f"- binaries with any label: **{fp}**",
    f"- false-positive rate: **{rate:.3f}**",
    "",
    "A4: confidences are **not fitted**. The table below is an observation",
    "on this negative set only. Detector constants were not changed.",
    "",
]
if calib:
    md += [
        "## False-positive confidence (A4 observation)",
        "",
        "| Detection | n | mean conf | min | max |",
        "|-----------|---|-----------|-----|-----|",
    ]
    for row in calib:
        md.append(
            f"| `{row['key']}` | {row['n']} | {row['mean_confidence']:.3f} | "
            f"{row['min_confidence']:.3f} | {row['max_confidence']:.3f} |"
        )
    md.append("")
if pos:
    md += ["## Positives (false positives)", ""]
    for name in sorted(pos):
        md.append(f"- `{name}`: {', '.join(pos[name])}")
    md.append("")
else:
    md += ["No labels extracted (0 false positives on this run).", ""]

(root / "results/b8-loop-negatives.md").write_text("\n".join(md), encoding="utf-8")
summary = {
    "requested": n,
    "decompiled": ok,
    "false_positive_binaries": fp,
    "false_positive_rate": rate,
    "positives": pos,
    "calibration_observation": calib,
    "fitted": False,
}
(root / "results/b8-loop-negatives.json").write_text(
    json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
print(f"B8_LOOP_OK requested={n} decompiled={ok} fp={fp} rate={rate:.3f}")
PY
