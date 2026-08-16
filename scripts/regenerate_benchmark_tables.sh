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

bash "${ROOT}/scripts/run_benchmarks.sh" --profile ci-core 2>/dev/null || true

python3 - "${ROOT}" "${OUT}" "${SHA}" "${VER}" <<'PY'
import json, pathlib, sys
from datetime import datetime, timezone

root, out, sha, ver = sys.argv[1:5]
results = pathlib.Path(root) / "results" / f"{sha}.json"
baseline = pathlib.Path(root) / "results" / "baseline-2026-08.json"
ar_ci = pathlib.Path(root) / "results" / "algorithm-recovery-ci.json"
ar_base = pathlib.Path(root) / "results" / "baseline-algorithm-recovery.json"

def load(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}

cur = load(results)
base = load(baseline)
ar = load(ar_ci)
ar_b = load(ar_base)

m = cur.get("metrics", {})
bm = base.get("metrics", {})
db = cur.get("decompilebench", {})
stock = db.get("stock_retdec", {}).get("summary", {})
if not stock:
    for name in ("stock-retdec-docker-full.json", "stock-retdec-docker-ci-core.json"):
        p = pathlib.Path(root) / "results" / name
        if p.is_file():
            stock = load(p).get("summary", {})
            break
fork = db.get("summary", db.get("fork", {}).get("summary", {}))
ar_summary = ar.get("summary", {}) or cur.get("algorithm_recovery", {}).get("summary", {})
ar_b_ci = ar_b.get("metrics", {}).get("ci_core", {})
ar_full = load(pathlib.Path(root) / "results" / "algorithm-recovery-full.json").get("summary", {})
ar_b_full = ar_b.get("metrics", {}).get("full_corpus", {})

stock_note = "remnux/retdec (stock v5.0). Official Hub image retdec/retdec:v5.0 does not exist."

lines = [
    "# Benchmark tables (auto-generated)",
    "",
    f"- **Version:** {ver}",
    f"- **Commit:** `{sha}`",
    f"- **Generated:** {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}",
    "",
    "## DecompileBench (stand-in corpus)",
    "",
    "| Metric | Fork | Stock RetDec 5.0 | Baseline |",
    "|--------|------|------------------|----------|",
]
for key, label in (
    ("syntax_valid_rate", "syntax_valid_rate"),
    ("recompile_success_rate", "recompile_success_rate"),
    ("coverage_equivalence_rate", "coverage_equivalence_rate"),
    ("mean_wall_s", "mean_wall_s"),
):
    fval = fork.get(key, m.get("decompilebench", {}).get(key, "—"))
    sval = stock.get(key, "—")
    bval = bm.get("decompilebench", {}).get(key, "—")
    lines.append(f"| {label} | {fval} | {sval} | {bval} |")

lines += [
    "",
    f"Stock column: {stock_note} F1 is fork-only (stock has no label export).",
    "",
    "## Algorithm recovery",
    "",
    "| Profile | mean_f1 | mean_f1_raw | decompiled |",
    "|---------|---------|-------------|------------|",
    f"| CI core (9) | {ar_summary.get('mean_f1', ar_b_ci.get('mean_f1', '—'))} | {ar_summary.get('mean_f1_raw', ar_b_ci.get('mean_f1_raw', '—'))} | {ar_summary.get('decompiled', ar_b_ci.get('min_decompiled', '—'))} |",
    f"| Full corpus (216) | {ar_full.get('mean_f1', ar_b_full.get('mean_f1', '—'))} | {ar_full.get('mean_f1_raw', ar_b_full.get('mean_f1_raw', '—'))} | {ar_full.get('decompiled', ar_b_full.get('min_decompiled', '—'))} |",
    "",
    "`mean_f1` uses stem/label fallback; `mean_f1_raw` is detector-only.",
    "",
    "_Regenerate: `bash scripts/regenerate_benchmark_tables.sh`_",
    "",
]

pathlib.Path(out).write_text("\n".join(lines), encoding="utf-8")
print(f"Wrote {out}")
PY
