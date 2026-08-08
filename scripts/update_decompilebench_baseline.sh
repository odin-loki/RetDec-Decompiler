#!/usr/bin/env bash
# update_decompilebench_baseline.sh — Refresh results/baseline-2026-08.json from a run.
# Usage: bash scripts/update_decompilebench_baseline.sh --from results/<sha>.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FROM=""
BASELINE="${ROOT}/results/baseline-2026-08.json"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--from) FROM="$2"; shift 2 ;;
		--baseline) BASELINE="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${FROM}" || ! -f "${FROM}" ]]; then
	echo "Missing --from results file" >&2
	exit 1
fi

python3 - "${FROM}" "${BASELINE}" <<'PY'
import json, sys
from datetime import datetime, timezone

src, dst = map(__import__("pathlib").Path, sys.argv[1:3])
cur = json.loads(src.read_text(encoding="utf-8"))
metrics = cur.get("metrics", {})
db = metrics.get("decompilebench", {})
ar = metrics.get("algorithm_recovery", {})

out = {
    "baseline_tag": "baseline-2026-08",
    "updated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "note": "Live metrics from run_benchmarks.sh (algorithm-recovery stand-in corpus).",
    "metrics": {
        "decompilebench": {
            "syntax_valid_rate": db.get("syntax_valid_rate", 0.0),
            "recompile_success_rate": db.get("recompile_success_rate", 0.0),
            "coverage_equivalence_rate": db.get("coverage_equivalence_rate"),
        },
        "algorithm_recovery": {
            "mean_f1": ar.get("mean_f1", 0.0),
            "mean_f1_raw": ar.get("mean_f1_raw"),
        },
    },
    "thresholds": {
        "syntax_valid_drop_max": 0.05,
        "recompile_success_drop_max": 0.05,
        "coverage_equivalence_drop_max": 0.05,
        "mean_f1_drop_max": 0.05,
    },
}
dst.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
print(f"Updated {dst}")
PY
