#!/usr/bin/env bash
# eval_rellic.sh — rellic evaluation scaffold (step 28).
# Usage: bash scripts/eval_rellic.sh [--corpus DIR] [--out FILE]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS="${ROOT}/tests/decompilebench/corpus"
OUT="${ROOT}/results/rellic-eval.json"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--corpus) CORPUS="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "$(dirname "${OUT}")"

if ! command -v rellic-decompile >/dev/null 2>&1; then
	cat > "${OUT}" <<EOF
{
  "status": "blocked",
  "reason": "rellic-decompile not on PATH",
  "next": "Build rellic; see docs/internal/rellic_evaluation.md"
}
EOF
	echo "rellic-decompile not found — wrote blocked status to ${OUT}"
	exit 0
fi

python3 - "${CORPUS}" "${OUT}" <<'PY'
import json, pathlib, subprocess, sys, time
corpus, out = map(pathlib.Path, sys.argv[1:3])
rows = []
for sample in sorted(corpus.glob("*")) if corpus.is_dir() else []:
    if not sample.is_file():
        continue
    t0 = time.time()
    proc = subprocess.run(
        ["rellic-decompile", str(sample)],
        capture_output=True, text=True,
    )
    rows.append({
        "input": str(sample),
        "exit_code": proc.returncode,
        "wall_s": round(time.time() - t0, 3),
    })
payload = {
    "status": "ok" if rows else "empty_corpus",
    "harness": "rellic-eval",
    "samples": rows,
}
out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {out} ({len(rows)} samples)")
PY
