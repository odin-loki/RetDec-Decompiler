#!/usr/bin/env bash
# eval_lief.sh — LIEF adoption evaluation scaffold (step 29).
# Usage: bash scripts/eval_lief.sh [--corpus DIR] [--out FILE]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS="${ROOT}/tests/algorithm_recovery/corpus"
OUT="${ROOT}/results/lief-eval.json"
LIMIT=20
PYTHON="$("${ROOT}/scripts/find_python.sh")"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--corpus) CORPUS="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--limit) LIMIT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "$(dirname "${OUT}")"

"${PYTHON}" - "${CORPUS}" "${OUT}" "${LIMIT}" <<'PY'
import json, pathlib, subprocess, sys

corpus = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
limit = int(sys.argv[3])

try:
    import lief  # type: ignore
    has_lief = True
except ImportError:
    has_lief = False

rows = []
samples = sorted(p for p in corpus.iterdir() if p.is_file() and p.suffix != ".json")[:limit]

for sample in samples:
    row = {"input": sample.name, "readelf_sections": None, "lief_sections": None, "match": None}
    proc = subprocess.run(
        ["readelf", "-S", str(sample)],
        capture_output=True, text=True,
    )
    if proc.returncode == 0:
        row["readelf_sections"] = proc.stdout.count("] ")
    if has_lief:
        try:
            binary = lief.parse(str(sample))
            row["lief_sections"] = len(binary.sections) if binary else 0
            if row["readelf_sections"] is not None and row["lief_sections"] is not None:
                row["match"] = row["readelf_sections"] == row["lief_sections"]
        except Exception as exc:
            row["lief_error"] = str(exc)
    rows.append(row)

payload = {
    "status": "blocked" if not has_lief else ("ok" if rows else "empty_corpus"),
    "harness": "lief-eval",
    "lief_available": has_lief,
    "retdec_has_lief": False,
    "samples": rows,
    "next": "Build with RETDEC_ENABLE_LIEF=ON; see docs/internal/lief_adoption.md",
}
if not has_lief:
    payload["reason"] = "python lief package not installed (pip install -r scripts/requirements-eval.txt)"

out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {out} ({len(rows)} samples, lief={has_lief})")
PY
