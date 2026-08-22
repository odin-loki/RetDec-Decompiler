#!/usr/bin/env bash
# Measure goto counts in default F5 .c on the ci-core 9 (audit Q4 baseline).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
CORPUS="${ROOT}/tests/algorithm_recovery/corpus"
WORK="${ROOT}/build/linux/q4-goto"
if [[ ! -x "${DEC}" ]]; then
	echo "missing ${DEC}" >&2
	exit 1
fi
NAMES=(
	bubblesort-gcc-O0
	mergesort-gcc-O0
	hash_table-gcc-O0
	ring_buffer-gcc-O0
	binary_search-gcc-O0
	memcpy_loop-gcc-O0
	generated_quicksort-gcc-O0
	generated_heapsort-gcc-O0
	generated_pthread_mutex-gcc-O0
)
DEC_DIR="$(cd "$(dirname "${DEC}")" && pwd)"
SHARE_DIR="$(cd "${DEC_DIR}/.." && pwd)/share/retdec"
mkdir -p "${SHARE_DIR}/profiles" "${WORK}"
cp -f "${ROOT}/src/retdec-decompiler/decompiler-config.json" "${SHARE_DIR}/"
python3 - "${DEC}" "${CORPUS}" "${WORK}" "${ROOT}" "${NAMES[*]}" <<'PY'
import json, os, re, subprocess, sys
from pathlib import Path

dec, corpus, work, root = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
names = sys.argv[5].split()
goto_re = re.compile(r"\bgoto\b")
rows = []
for name in names:
    src = corpus / name
    if not src.is_file():
        print("missing", src, file=sys.stderr)
        continue
    out_c = work / f"{name}.c"
    env = os.environ.copy()
    env.pop("RETDEC_EMIT_BUILDABLE", None)
    proc = subprocess.run(
        [str(dec), str(src), "--output", str(out_c)],
        capture_output=True, text=True, timeout=180, env=env,
    )
    if proc.returncode != 0 or not out_c.is_file():
        print("decompile fail", name, file=sys.stderr)
        continue
    text = out_c.read_text(encoding="utf-8", errors="replace")
    n = len(goto_re.findall(text))
    rows.append({"name": name, "goto_count": n, "bytes": len(text)})

n = len(rows)
total = sum(r["goto_count"] for r in rows)
mean = (total / n) if n else 0.0
payload = {"samples": rows, "count": n, "total_goto": total, "mean_goto": mean}
(root / "results/goto-optimizer-baseline.json").write_text(
    json.dumps(payload, indent=2) + "\n", encoding="utf-8"
)
md = [
    "# Q4 goto-optimizer baseline",
    "",
    "Default F5 `.c` (not `.buildable.c`) on ci-core 9. Counts the `goto` token.",
    "This is the pre-SAILR baseline; do not treat it as a SAILR port.",
    "",
    f"- samples: {n}",
    f"- total goto: **{total}**",
    f"- mean goto: **{mean:.2f}**",
    "",
    "| Binary | goto | bytes |",
    "|--------|------|-------|",
]
for r in rows:
    md.append(f"| `{r['name']}` | {r['goto_count']} | {r['bytes']} |")
md.append("")
(root / "results/goto-optimizer-baseline.md").write_text("\n".join(md), encoding="utf-8")
print(f"Q4_OK n={n} total_goto={total} mean={mean:.2f}")
PY
