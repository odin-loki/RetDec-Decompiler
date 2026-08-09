#!/usr/bin/env bash
# automation_status.sh — Print MASTER-UPGRADE-PLAN automation completion and blockers.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="$("${ROOT}/scripts/find_python.sh")"
VER="$(grep -E '^[[:space:]]*VERSION[[:space:]]' "${ROOT}/CMakeLists.txt" | head -1 | sed -E 's/.*VERSION[[:space:]]+([0-9.]+).*/\1/')"

echo "RetDec automation status (v${VER})"
echo "=================================="
echo ""

check() {
	if "$@" >/dev/null 2>&1; then
		echo "  OK   $*"
	else
		echo "  --   $*"
	fi
}

echo "Steps 1–26 (shippable): DONE"
echo "  Releases: v1.0.0–v${VER} on main"
if [[ -f "${ROOT}/results/baseline-algorithm-recovery.json" ]]; then
	"${PYTHON}" - "${ROOT}/results/baseline-algorithm-recovery.json" <<'PY'
import json, sys
b = json.load(open(sys.argv[1], encoding="utf-8"))
for profile, m in b.get("metrics", {}).items():
    raw = m.get("mean_f1_raw")
    extra = f" mean_f1_raw={raw}" if raw is not None else ""
    print(f"  F1 baseline {profile}: decompiled={m.get('min_decompiled')} mean_f1={m.get('mean_f1')}{extra}")
PY
fi
echo ""

echo "Steps 27–33 (roadmap): SCAFFOLDED (optional research — see docs/internal/MAINTAINER_SCOPE.md)"
if [[ -f "${ROOT}/results/migration-eval-summary.json" ]]; then
	echo "  Migration evals:"
	"${PYTHON}" -c "import json; d=json.load(open('${ROOT}/results/migration-eval-summary.json')); print('   ', d.get('evals',{}))"
fi
echo ""

echo "Environment checks (optional):"
check command -v gh
if command -v gh >/dev/null 2>&1; then
	gh auth status >/dev/null 2>&1 && echo "  OK   gh authenticated (Windows)" || echo "  --   gh auth login  (PowerShell; optional)"
fi
check test -x "${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
DEC="$(find "${ROOT}/build" -name 'retdec-decompiler' -type f 2>/dev/null | head -n1 || true)"
[[ -n "${DEC}" ]] && echo "  OK   decompiler: ${DEC}" || echo "  --   decompiler not built"
"${PYTHON}" -c "import lief" 2>/dev/null && echo "  OK   python lief (eval venv)" || echo "  --   bash scripts/setup_eval_venv.sh"
command -v rellic-decompile >/dev/null 2>&1 && echo "  OK   rellic-decompile" || echo "  --   rellic (optional research)"
command -v retypd >/dev/null 2>&1 && echo "  OK   retypd" || echo "  --   retypd (optional research)"
echo ""

echo "Out of scope (documented): Docker, OSS-Fuzz 23k corpus, four-toolchain support regen"
echo ""

echo "Runnable now:"
echo "  bash scripts/run_all_automation.sh --skip-support-regen"
echo "  bash scripts/analyze_full_f1.py"
echo "  bash scripts/install_lief_sdk.sh"
echo "  bash scripts/verify_lief_build.sh"
echo ""
echo "Git/CI (Windows PowerShell only):"
echo "  gh auth login"
echo "  .\\scripts\\dispatch_algorithm_recovery_nightly.ps1"
echo ""
echo "See docs/internal/MAINTAINER_SCOPE.md"
