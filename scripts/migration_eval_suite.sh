#!/usr/bin/env bash
# migration_eval_suite.sh — Run library migration eval scaffolds (steps 28–31).
# Usage: bash scripts/migration_eval_suite.sh [--decompiler PATH]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEC=""
CORPUS="${ROOT}/tests/algorithm_recovery/corpus"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--corpus) CORPUS="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "${ROOT}/results"

echo "==> rellic"
bash "${ROOT}/scripts/eval_rellic.sh" --corpus "${CORPUS}"

echo "==> LIEF"
bash "${ROOT}/scripts/eval_lief.sh" --corpus "${CORPUS}"

echo "==> Retypd"
bash "${ROOT}/scripts/eval_retypd.sh" --corpus "${CORPUS}"

if [[ -n "${DEC}" ]]; then
	echo "==> SAILR (goto metrics)"
	bash "${ROOT}/scripts/eval_sailr.sh" --decompiler "${DEC}" --corpus "${CORPUS}"
else
	echo "==> SAILR (skipped — no decompiler)"
fi

echo "==> LLVM API inventory"
bash "${ROOT}/scripts/inventory_llvm_apis.sh"

python3 - "${ROOT}/results" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
summary = {}
for name in ("rellic-eval.json", "lief-eval.json", "retypd-eval.json", "sailr-eval.json", "llvm-api-inventory.json"):
    path = root / name
    if path.is_file():
        data = json.loads(path.read_text(encoding="utf-8"))
        summary[name] = data.get("status", data.get("harness", "ok"))
out = root / "migration-eval-summary.json"
out.write_text(json.dumps({"harness": "migration_eval_suite", "evals": summary}, indent=2) + "\n")
print(f"Wrote {out}")
PY

echo "Migration eval suite complete."
