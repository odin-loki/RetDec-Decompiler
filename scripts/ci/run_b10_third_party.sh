#!/usr/bin/env bash
# Name-blind score of the B10 third-party (zlib) binaries.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
CORPUS="${ROOT}/tests/algorithm_recovery/third_party_corpus"
if [[ ! -x "${DEC}" ]]; then
	echo "missing ${DEC}" >&2
	exit 1
fi
if [[ ! -f "${CORPUS}/manifest.json" ]]; then
	bash "${ROOT}/scripts/build_third_party_corpus.sh"
fi
DEC_DIR="$(cd "$(dirname "${DEC}")" && pwd)"
SHARE_DIR="$(cd "${DEC_DIR}/.." && pwd)/share/retdec"
mkdir -p "${SHARE_DIR}/profiles"
cp -f "${ROOT}/src/retdec-decompiler/decompiler-config.json" "${SHARE_DIR}/"
python3 "${ROOT}/scripts/extract_decompiler_predictions.py" \
	--decompiler "${DEC}" \
	--corpus "${CORPUS}" \
	--manifest "${CORPUS}/manifest.json" \
	--names "zlib_crc_only-gcc-O0,zlib_crc_only-gcc-O2" \
	--out "${ROOT}/tests/algorithm_recovery/predictions/third-party-nameblind.json" \
	--work "${ROOT}/build/linux/b10-work" \
	--jobs "$(nproc)" \
	--timeout 180 \
	--no-stem-fallback
python3 "${ROOT}/tests/algorithm_recovery/runner.py" \
	--predictions "${ROOT}/tests/algorithm_recovery/predictions/third-party-nameblind.json" \
	--ground-truth "${ROOT}/tests/algorithm_recovery/ground_truth/third_party.json" \
	--out "${ROOT}/results/algorithm-recovery-third-party-b10.json"
python3 - "${ROOT}" <<'PY'
import json
from pathlib import Path

root = Path(__import__("sys").argv[1])
score = json.loads((root / "results/algorithm-recovery-third-party-b10.json").read_text(encoding="utf-8"))
pred = json.loads((root / "tests/algorithm_recovery/predictions/third-party-nameblind.json").read_text(encoding="utf-8"))
truth = json.loads((root / "tests/algorithm_recovery/ground_truth/third_party.json").read_text(encoding="utf-8"))
preds = pred.get("predictions") or {}
mean = float(score["summary"]["mean_f1"])
micro = score.get("micro") or {}
n = int(score["summary"]["binaries"])
lines = [
    "# B10 third-party corpus",
    "",
    "Headline this run: zlib 1.3.1 **crc32.c only** (no deflate/trees).",
    "Labels are taken from that upstream source, not from the detector.",
    "The larger crc+deflate pair previously **timed out at 300s** and is",
    "not re-run here. This is **not** a full Debian coreutils/OpenSSL/SQLite set.",
    "",
    "Name-blind (`--no-stem-fallback`). CRC is not an assigned `IdiomDetector`",
    "kind (same rule as A6). Expect low recall.",
    "",
    f"- binaries: **{n}**",
    f"- mean F1: **{mean:.3f}**",
    f"- micro F1: **{float(micro.get('f1', 0.0)):.3f}** (tp={micro.get('tp')} fp={micro.get('fp')} fn={micro.get('fn')})",
    "",
    "| Binary | Expected | Predicted | F1 |",
    "|--------|----------|-----------|----|",
]
per = score.get("per_binary") or {}
for name in sorted(preds or truth):
    exp = ", ".join(truth.get(name) or []) or "(none)"
    got = ", ".join(preds.get(name) or []) or "(none)"
    f1 = float((per.get(name) or {}).get("f1", 0.0))
    lines.append(f"| `{name}` | {exp} | {got} | {f1:.3f} |")
lines.append("")
(root / "results/b10-third-party.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"B10_OK n={n} mean_f1={mean:.3f}")
PY
