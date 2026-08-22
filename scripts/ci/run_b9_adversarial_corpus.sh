#!/usr/bin/env bash
# Decompile the B9 adversarial-positive corpus and publish name-blind recall.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
CORPUS="${ROOT}/tests/algorithm_recovery/adversarial_corpus"
if [[ ! -x "${DEC}" ]]; then
	echo "missing ${DEC}" >&2
	exit 1
fi
if [[ ! -f "${CORPUS}/manifest.json" ]]; then
	bash "${ROOT}/scripts/build_adversarial_corpus.sh"
fi
DEC_DIR="$(cd "$(dirname "${DEC}")" && pwd)"
SHARE_DIR="$(cd "${DEC_DIR}/.." && pwd)/share/retdec"
mkdir -p "${SHARE_DIR}/profiles"
cp -f "${ROOT}/src/retdec-decompiler/decompiler-config.json" "${SHARE_DIR}/"
python3 "${ROOT}/scripts/extract_decompiler_predictions.py" \
	--decompiler "${DEC}" \
	--corpus "${CORPUS}" \
	--manifest "${CORPUS}/manifest.json" \
	--out "${ROOT}/tests/algorithm_recovery/predictions/adversarial-nameblind.json" \
	--work "${ROOT}/build/linux/b9-work" \
	--jobs "$(nproc)" \
	--timeout 180 \
	--no-stem-fallback
python3 "${ROOT}/tests/algorithm_recovery/runner.py" \
	--predictions "${ROOT}/tests/algorithm_recovery/predictions/adversarial-nameblind.json" \
	--ground-truth "${ROOT}/tests/algorithm_recovery/ground_truth/adversarial.json" \
	--out "${ROOT}/results/algorithm-recovery-adversarial-b9.json"
python3 - "${ROOT}" <<'PY'
import json
from pathlib import Path

root = Path(__import__("sys").argv[1])
score = json.loads((root / "results/algorithm-recovery-adversarial-b9.json").read_text(encoding="utf-8"))
pred = json.loads((root / "tests/algorithm_recovery/predictions/adversarial-nameblind.json").read_text(encoding="utf-8"))
truth = json.loads((root / "tests/algorithm_recovery/ground_truth/adversarial.json").read_text(encoding="utf-8"))
preds = pred.get("predictions") or {}
work = root / "build/linux/b9-work"
aes_evidence = {}
for name in sorted(truth):
    if "aes_" not in name:
        continue
    cfg_path = work / name / f"{name}.c.config.json"
    if not cfg_path.is_file():
        cfg_path = work / f"{name}.exe" / f"{name}.exe.c.config.json"
    evidence = []
    if cfg_path.is_file():
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        for fn in cfg.get("functions") or []:
            for c in fn.get("usedCryptoConstants") or []:
                evidence.append(str(c))
            for det in fn.get("semanticDetections") or []:
                blob = f"{det.get('kind','')} {det.get('label','')}".lower()
                if "aes" in blob or det.get("kind") == "crypto":
                    evidence.append(f"{det.get('kind')}:{det.get('label')}")
    aes_evidence[name] = sorted(set(evidence))

mean = float(score["summary"]["mean_f1"])
micro = score.get("micro") or {}
n = int(score["summary"]["binaries"])
lines = [
    "# B9 adversarial-positive corpus",
    "",
    "Idiosyncratic implementations of target algorithms (audit B9):",
    "heapsort with a 1-based sentinel, BFS with a ring buffer, iterative DFS,",
    "table atoi, SWAR strlen, unrolled varint, AES T-tables, algebraic AES",
    "(GF inversion, no S-box array), and AES-NI.",
    "",
    "Scored name-blind (`--no-stem-fallback`). This is **recall on hard positives**,",
    "not a product F1. `crypto_detect` is **not** merged into decompiler",
    "`semanticDetections` (no public-header change to `FunctionDetections`).",
    "AES rows are therefore expected misses unless `usedCryptoConstants` is set.",
    "",
    f"- binaries: **{n}**",
    f"- mean F1: **{mean:.3f}**",
    f"- micro F1: **{float(micro.get('f1', 0.0)):.3f}** (tp={micro.get('tp')} fp={micro.get('fp')} fn={micro.get('fn')})",
    "",
    "## Per binary",
    "",
    "| Binary | Expected | Predicted | F1 |",
    "|--------|----------|-----------|----|",
]
per = score.get("per_binary") or {}
for name in sorted(truth):
    exp = ", ".join(truth.get(name) or []) or "(none)"
    got = ", ".join(preds.get(name) or []) or "(none)"
    f1 = float((per.get(name) or {}).get("f1", 0.0))
    lines.append(f"| `{name}` | {exp} | {got} | {f1:.3f} |")
lines += ["", "## AES evidence in `.config.json`", ""]
if aes_evidence:
    for name, ev in aes_evidence.items():
        lines.append(f"- `{name}`: {', '.join(ev) if ev else 'none'}")
else:
    lines.append("No AES binaries in this run.")
lines.append("")
(root / "results/b9-adversarial-positive.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"B9_OK n={n} mean_f1={mean:.3f} micro_f1={float(micro.get('f1', 0.0)):.3f}")
PY
