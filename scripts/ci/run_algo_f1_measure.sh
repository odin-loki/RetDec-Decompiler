#!/usr/bin/env bash
# Measure post-B1 algorithm F1. Does not run the 0.95 gate (withdrawn).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
PRED="${ROOT}/tests/algorithm_recovery/predictions/ci.json"
GT="${ROOT}/tests/algorithm_recovery/ground_truth/corpus.json"
RESULTS="${ROOT}/results/algorithm-recovery-ci.json"
WORK="${ROOT}/build/prediction-work"
mkdir -p "${ROOT}/results" "${ROOT}/tests/algorithm_recovery/predictions"
if [[ ! -f "${ROOT}/tests/algorithm_recovery/corpus/manifest.json" ]]; then
	bash "${ROOT}/scripts/build_algorithm_corpus.sh"
fi
python3 "${ROOT}/scripts/extract_decompiler_predictions.py" \
	--decompiler "${DEC}" \
	--corpus "${ROOT}/tests/algorithm_recovery/corpus" \
	--manifest "${ROOT}/tests/algorithm_recovery/corpus/manifest.json" \
	--ci-core \
	--no-stem-fallback \
	--work "${WORK}" \
	--out "${PRED}"
python3 "${ROOT}/tests/algorithm_recovery/runner.py" \
	--predictions "${PRED}" \
	--ground-truth "${GT}" \
	--out "${RESULTS}"
python3 - <<'PY'
import json
from pathlib import Path
p = Path("/mnt/c/Users/odinl/OneDrive/Desktop/RetDec/results/algorithm-recovery-ci.json")
d = json.loads(p.read_text(encoding="utf-8"))
print("mean_f1", d.get("mean_f1") or d.get("summary") or list(d.keys())[:20])
PY
echo ALGO_F1_OK
