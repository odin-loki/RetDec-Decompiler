#!/usr/bin/env bash
# eval_sailr.sh — SAILR / goto-structure evaluation scaffold (step 31).
# Usage: bash scripts/eval_sailr.sh [--decompiler PATH] [--corpus DIR] [--out FILE]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS="${ROOT}/tests/algorithm_recovery/corpus"
OUT="${ROOT}/results/sailr-eval.json"
DEC=""
LIMIT=6
PYTHON="$("${ROOT}/scripts/find_python.sh")"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--corpus) CORPUS="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--limit) LIMIT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${DEC}" ]]; then
	for candidate in \
		"${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler" \
		"${ROOT}/build/linux/bin/retdec-decompiler" \
		"$(find "${ROOT}/build/windows" -name 'retdec-decompiler.exe' -type f 2>/dev/null | head -n1)"; do
		if [[ -n "${candidate}" && -x "${candidate}" ]]; then
			DEC="${candidate}"
			break
		fi
	done
fi

mkdir -p "$(dirname "${OUT}")"

"${PYTHON}" - "${CORPUS}" "${OUT}" "${LIMIT}" "${DEC}" <<'PY'
import json, pathlib, re, subprocess, sys, tempfile

corpus = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
limit = int(sys.argv[3])
dec = sys.argv[4] or None

goto_re = re.compile(r"\bgoto\b")
samples = sorted(p for p in corpus.iterdir() if p.is_file() and p.suffix != ".json")[:limit]
rows = []

for sample in samples:
    row = {"input": sample.name, "goto_count": None, "lines": None}
    if not dec or not pathlib.Path(dec).is_file():
        row["status"] = "no_decompiler"
        rows.append(row)
        continue
    try:
        with tempfile.TemporaryDirectory() as td:
            out_c = pathlib.Path(td) / "out.c"
            proc = subprocess.run(
                [dec, str(sample), "-o", str(out_c)],
                capture_output=True, text=True,
            )
            row["decompile_rc"] = proc.returncode
            if out_c.is_file():
                text = out_c.read_text(encoding="utf-8", errors="replace")
                row["lines"] = text.count("\n") + 1
                row["goto_count"] = len(goto_re.findall(text))
                row["status"] = "ok"
            else:
                row["status"] = "no_output"
    except OSError as exc:
        row["status"] = "decompiler_exec_error"
        row["error"] = str(exc)
    rows.append(row)

mean_goto = sum(r.get("goto_count") or 0 for r in rows) / len(rows) if rows else 0.0
payload = {
    "status": "ok" if dec and rows else "blocked",
    "harness": "sailr-eval",
    "decompiler": dec,
    "mean_goto_count": round(mean_goto, 2),
    "samples": rows,
    "next": "Integrate SAILR-style structuring; see goto_cfg_optimizer.h",
}
if not dec:
    payload["reason"] = "retdec-decompiler not found"

out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {out} ({len(rows)} samples, mean_goto={mean_goto:.1f})")
PY
