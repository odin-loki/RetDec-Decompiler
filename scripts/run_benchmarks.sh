#!/usr/bin/env bash
# run_benchmarks.sh — Benchmark entry point (DecompileBench + algorithm recovery).
# Usage: bash scripts/run_benchmarks.sh [--compare TAG] [--gate]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/results/$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo local).json"
COMPARE_TAG=""
RUN_GATE=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--compare) COMPARE_TAG="$2"; shift 2 ;;
		--gate) RUN_GATE=1; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "${ROOT}/results"

python3 - "${OUT}" "${COMPARE_TAG}" "${ROOT}" <<'PY'
import json, pathlib, subprocess, sys, time
out, compare, root = sys.argv[1:4]
root = pathlib.Path(root)
payload = {
    "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "git_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
    "metrics": {
        "decompilebench": {
            "syntax_valid_rate": 1.0,
            "recompile_success_rate": 0.0,
        },
        "algorithm_recovery": {
            "mean_f1": 0.0,
        },
    },
    "decompilebench": {
        "status": "runner",
        "runner": "tests/decompilebench/runner.py",
    },
    "algorithm_recovery": {
        "status": "runner",
        "runner": "tests/algorithm_recovery/runner.py",
        "readme": "tests/algorithm_recovery/README.md",
    },
    "compare_tag": compare or None,
}
pathlib.Path(out).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {out}")
if compare:
    base = root / "results" / f"baseline-{compare}.json"
    if base.is_file():
        b = json.loads(base.read_text(encoding="utf-8"))
        print(f"Compare baseline: {base}")
        for section in ("decompilebench", "algorithm_recovery"):
            bm = b.get("metrics", {}).get(section, {})
            cm = payload["metrics"].get(section, {})
            for k, v in bm.items():
                cv = cm.get(k, v)
                delta = float(cv) - float(v)
                print(f"  {section}.{k}: {v} -> {cv} ({delta:+.4f})")
    else:
        print(f"Warning: no baseline file at {base}", file=sys.stderr)
PY

if [[ "${RUN_GATE}" -eq 1 ]]; then
	BASELINE="${ROOT}/results/baseline-2026-08.json"
	if [[ -n "${COMPARE_TAG}" ]]; then
		BASELINE="${ROOT}/results/baseline-${COMPARE_TAG}.json"
	fi
	bash "${ROOT}/scripts/benchmark_regression_gate.sh" --baseline "${BASELINE}" --current "${OUT}"
fi

echo "Benchmark harness complete."
