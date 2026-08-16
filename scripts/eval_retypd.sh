#!/usr/bin/env bash
# eval_retypd.sh — Retypd evaluation scaffold (step 30).
# Usage: bash scripts/eval_retypd.sh [--corpus DIR] [--out FILE]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS="${ROOT}/tests/algorithm_recovery/corpus"
OUT="${ROOT}/results/retypd-eval.json"
LIMIT=10
PYTHON="$(bash "${ROOT}/scripts/find_python.sh")"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--corpus) CORPUS="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--limit) LIMIT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "$(dirname "${OUT}")"

"${PYTHON}" - "${CORPUS}" "${OUT}" "${LIMIT}" "${ROOT}" <<'PY'
import json, pathlib, subprocess, sys

corpus = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
limit = int(sys.argv[3])
root = pathlib.Path(sys.argv[4])

has_retypd = bool(subprocess.run(["which", "retypd"], capture_output=True).returncode == 0)
dec_candidates = list((root / "build/linux").rglob("retdec-decompiler")) if (root / "build/linux").is_dir() else []
dec = next((str(p) for p in dec_candidates if p.is_file()), None)

rows = []
samples = sorted(p for p in corpus.iterdir() if p.is_file() and p.suffix != ".json")[:limit]

for sample in samples:
    row = {"input": sample.name, "llvm_artifact": None, "retypd_status": "skipped"}
    if dec:
        with __import__("tempfile").TemporaryDirectory() as td:
            out_c = pathlib.Path(td) / "out.c"
            proc = subprocess.run(
                [dec, str(sample), "-o", str(out_c)],
                capture_output=True, text=True,
            )
            row["decompile_rc"] = proc.returncode
            for ext in (".bc", ".ll"):
                candidate = out_c.with_suffix(ext)
                if candidate.is_file():
                    row["llvm_artifact"] = str(candidate)
                    break
            if not row["llvm_artifact"]:
                row["llvm_artifact"] = str(out_c.with_suffix(".ll")) if out_c.with_suffix(".ll").is_file() else None
    if has_retypd and row.get("llvm_artifact") and pathlib.Path(row["llvm_artifact"]).is_file():
        proc = subprocess.run(["retypd", row["llvm_artifact"]], capture_output=True, text=True)
        row["retypd_status"] = "ok" if proc.returncode == 0 else "fail"
        row["retypd_rc"] = proc.returncode
    rows.append(row)

payload = {
    "status": "blocked" if not has_retypd else ("ok" if rows else "empty_corpus"),
    "harness": "retypd-eval",
    "retypd_available": has_retypd,
    "decompiler": dec,
    "samples": rows,
    "next": "Spike Retypd on 10 LLVM modules; see docs/internal/retypd_sailr_llvm.md",
}
if not has_retypd:
    payload["reason"] = "retypd not on PATH"

out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {out} ({len(rows)} samples, retypd={has_retypd})")
PY
