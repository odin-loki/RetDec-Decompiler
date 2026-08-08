#!/usr/bin/env bash
# nightly_report.sh — Generate docs/internal/nightly report (Part 16.5).
# Usage: bash scripts/nightly_report.sh [--out FILE]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATE="$(date -u +%Y-%m-%d)"
OUT="${ROOT}/docs/internal/nightly/${DATE}.md"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--out) OUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "$(dirname "${OUT}")"

SHA="$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo unknown)"
VER="$(grep -E '^[[:space:]]*VERSION[[:space:]]' "${ROOT}/CMakeLists.txt" | head -1 | sed -E 's/.*VERSION[[:space:]]+([0-9.]+).*/\1/')"

{
	echo "# Nightly report — ${DATE}"
	echo ""
	echo "- **Commit:** \`${SHA}\`"
	echo "- **Version:** ${VER}"
	echo "- **Generated:** $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo ""
	echo "## Benchmarks"
	if [[ -f "${ROOT}/results/${SHA}.json" ]]; then
		echo "- Latest: \`results/${SHA}.json\`"
	else
		echo "- No results for current SHA (run \`bash scripts/run_benchmarks.sh\`)"
	fi
	echo "- Baseline: \`results/baseline-2026-08.json\`"
	echo ""
	echo "## Performance"
	if [[ -f perf_bench_result.json ]]; then
		echo '```json'
		cat perf_bench_result.json
		echo '```'
	else
		echo "- No \`perf_bench_result.json\` in cwd"
	fi
	echo ""
	echo "## Doctor"
	echo '```'
	bash "${ROOT}/scripts/doctor.sh" 2>&1 | tail -n 5 || true
	echo '```'
	echo ""
	echo "## Open regressions"
	echo "- (manual) — link failing CI runs or fuzz crashes here"
} > "${OUT}"

echo "Wrote ${OUT}"
