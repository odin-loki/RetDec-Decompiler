#!/usr/bin/env bash
# run_benchmarks.sh — Benchmark entry point (DecompileBench + algorithm recovery).
# Usage: bash scripts/run_benchmarks.sh [--compare TAG]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/results/$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo local).json"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--compare) COMPARE_TAG="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "${ROOT}/results"

python3 - "${OUT}" "${COMPARE_TAG}" "${ROOT}" <<'PY'
import json, pathlib, subprocess, sys, time
out, compare, root = sys.argv[1:4]
root = pathlib.Path(root)
# Placeholder schema until DecompileBench corpus is wired (Phase 6.1).
payload = {
    "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "git_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
    "decompilebench": {
        "status": "pending",
        "note": "Wire tests/decompilebench runner in Phase 6.1",
    },
    "algorithm_recovery": {
        "status": "pending",
        "note": "Define corpus and ground truth in Phase 6.2",
    },
    "compare_tag": compare or None,
}
pathlib.Path(out).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {out}")
PY

echo "Benchmark harness placeholder complete."
