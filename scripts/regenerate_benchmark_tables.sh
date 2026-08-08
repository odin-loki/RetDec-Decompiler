#!/usr/bin/env bash
# regenerate_benchmark_tables.sh — Regenerate benchmark summary for releases (Part 12.4).
# Usage: bash scripts/regenerate_benchmark_tables.sh [--out FILE]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/docs/BENCHMARKS_TABLE.md"
SHA="$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo local)"
VER="$(grep -E '^[[:space:]]*VERSION[[:space:]]' "${ROOT}/CMakeLists.txt" | head -1 | sed -E 's/.*VERSION[[:space:]]+([0-9.]+).*/\1/')"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--out) OUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "$(dirname "${OUT}")"

# Best-effort: run harness if decompiler absent results still generated
bash "${ROOT}/scripts/run_benchmarks.sh" 2>/dev/null || true

python3 - "${ROOT}" "${OUT}" "${SHA}" "${VER}" <<'PY'
import json, pathlib, sys
from datetime import datetime, timezone

root, out, sha, ver = sys.argv[1:5]
results = root / "results" / f"{sha}.json"
baseline = root / "results" / "baseline-2026-08.json"
ar_ci = root / "results" / "algorithm-recovery-ci.json"
ar_base = root / "results" / "baseline-algorithm-recovery.json"

def load(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}

cur = load(pathlib.Path(results))
base = load(pathlib.Path(baseline))
ar = load(pathlib.Path(ar_ci))
ar_b = load(pathlib.Path(ar_base))

m = cur.get("metrics", {})
bm = base.get("metrics", {})
ar_summary = ar.get("summary", {})
ar_b_ci = ar_b.get("metrics", {}).get("ci_core", {})

lines = [
    "# Benchmark tables (auto-generated)",
    "",
    f"- **Version:** {ver}",
    f"- **Commit:** `{sha}`",
    f"- **Generated:** {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}",
    "",
    "## DecompileBench",
    "",
    "| Metric | Current | Baseline |",
    "|--------|---------|----------|",
]
for key in ("syntax_valid_rate", "recompile_success_rate"):
    c = m.get("decompilebench", {}).get(key, "—")
    b = bm.get("decompilebench", {}).get(key, "—")
    lines.append(f"| {key} | {c} | {b} |")

lines += [
    "",
    "## Algorithm recovery (CI core)",
    "",
    "| Metric | Current | Baseline |",
    "|--------|---------|----------|",
    f"| mean_f1 | {ar_summary.get('mean_f1', '—')} | {ar_b_ci.get('mean_f1', '—')} |",
    f"| decompiled | {ar_summary.get('decompiled', '—')} | {ar_b_ci.get('min_decompiled', '—')} |",
    "",
    "_Regenerate: `bash scripts/regenerate_benchmark_tables.sh`_",
    "",
]

pathlib.Path(out).write_text("\n".join(lines), encoding="utf-8")
print(f"Wrote {out}")
PY
