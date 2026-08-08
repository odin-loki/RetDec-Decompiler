#!/usr/bin/env bash
# update_algorithm_recovery_baseline.sh — Refresh baseline from a results file.
# Usage: bash scripts/update_algorithm_recovery_baseline.sh --from results/algorithm-recovery-ci.json [--profile ci_core]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FROM=""
PROFILE="ci_core"
OUT="${ROOT}/results/baseline-algorithm-recovery.json"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--from) FROM="$2"; shift 2 ;;
		--profile) PROFILE="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${FROM}" || ! -f "${FROM}" ]]; then
	echo "Missing --from results file" >&2
	exit 1
fi

python3 - "${FROM}" "${OUT}" "${PROFILE}" <<'PY'
import json, sys
from datetime import datetime, timezone

src, out, profile = sys.argv[1:4]
data = json.load(open(src, encoding="utf-8"))
summary = data.get("summary", {})
baseline = {}
if __import__("pathlib").Path(out).is_file():
    baseline = json.load(open(out, encoding="utf-8"))

metrics = baseline.setdefault("metrics", {})
metrics.setdefault(profile, {})
metrics[profile]["min_decompiled"] = summary.get("decompiled", metrics[profile].get("min_decompiled", 0))
metrics[profile]["mean_f1"] = summary.get("mean_f1", metrics[profile].get("mean_f1", 0.0))
if summary.get("mean_f1_raw") is not None:
    metrics[profile]["mean_f1_raw"] = summary.get("mean_f1_raw")
metrics[profile]["binaries"] = summary.get("binaries", metrics[profile].get("binaries", 0))
baseline["updated_at"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
baseline["baseline_tag"] = "baseline-algorithm-recovery"
baseline.setdefault("thresholds", {
    "mean_f1_drop_max": 0.05,
    "decompiled_drop_max": 10,
})

with open(out, "w", encoding="utf-8") as fh:
    json.dump(baseline, fh, indent=2)
    fh.write("\n")
print(f"Updated {out} profile={profile} decompiled={metrics[profile]['min_decompiled']} mean_f1={metrics[profile]['mean_f1']:.4f}")
PY
