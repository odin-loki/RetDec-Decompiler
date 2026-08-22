#!/usr/bin/env bash
# B6: name-blind labels must match after copy to $(sha256).bin (ci-core 9).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
CORPUS="${ROOT}/tests/algorithm_recovery/corpus"
WORK="${ROOT}/build/linux/b6-rename"
TIMEOUT=180
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
if [[ ! -x "${DEC}" ]]; then
	echo "missing ${DEC}" >&2
	exit 1
fi
DEC_DIR="$(cd "$(dirname "${DEC}")" && pwd)"
SHARE_DIR="$(cd "${DEC_DIR}/.." && pwd)/share/retdec"
mkdir -p "${SHARE_DIR}/profiles" "${WORK}/hashed"
cp -f "${ROOT}/src/retdec-decompiler/decompiler-config.json" "${SHARE_DIR}/"

python3 - "${DEC}" "${CORPUS}" "${WORK}" "${ROOT}" "${TIMEOUT}" "${NAMES[*]}" <<'PY'
import hashlib, json, os, subprocess, sys
from pathlib import Path

dec, corpus, work, root, timeout_s = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]), int(sys.argv[5])
names = sys.argv[6].split()

def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()

def decompile(src: Path, out_c: Path) -> bool:
    env = os.environ.copy()
    env["RETDEC_INCREMENTAL_CACHE"] = "0"
    env.pop("RETDEC_NEURAL_REFINE", None)
    try:
        proc = subprocess.run(
            [str(dec), str(src), "--output", str(out_c)],
            capture_output=True, text=True, timeout=timeout_s, env=env,
        )
    except subprocess.TimeoutExpired:
        return False
    return proc.returncode == 0 and out_c.is_file()

def labels_from_c(out_c: Path) -> set[str]:
    cands = [
        out_c.parent / (out_c.stem + ".config.json"),
        out_c.parent / (out_c.name + ".config.json"),
        Path(str(out_c) + ".config.json"),
    ]
    cfg_path = next((p for p in cands if p.is_file()), None)
    if cfg_path is None:
        return set()
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    labels = set()
    for fn in cfg.get("functions") or []:
        for det in fn.get("semanticDetections") or []:
            kind = (det.get("kind") or "").strip().lower()
            label = (det.get("label") or "").strip().lower()
            if kind or label:
                labels.add(f"{kind}:{label}")
    return labels

rows = []
fail = 0
for name in names:
    src = corpus / name
    if not src.is_file():
        print(f"missing {src}", file=sys.stderr)
        fail += 1
        continue
    digest = sha256(src)
    hashed = work / "hashed" / digest
    hashed.write_bytes(src.read_bytes())
    named_c = work / f"{name}.c"
    hash_c = work / "hashed" / f"{digest}.c"
    named_ok = decompile(src, named_c)
    hash_ok = decompile(hashed, hash_c)
    named_l = labels_from_c(named_c) if named_ok else set()
    hash_l = labels_from_c(hash_c) if hash_ok else set()
    identical = named_ok and hash_ok and named_l == hash_l
    if not named_ok or not hash_ok:
        fail += 1
    elif not identical:
        fail += 1
        print(f"COUPLING {name}: named={sorted(named_l)} hashed={sorted(hash_l)}", file=sys.stderr)
    rows.append({
        "name": name,
        "sha256": digest,
        "named_ok": named_ok,
        "hashed_ok": hash_ok,
        "identical": identical,
        "named_labels": sorted(named_l),
        "hashed_labels": sorted(hash_l),
    })

md = [
    "# B6 rename guard (ci-core 9)",
    "",
    "Each ELF is copied to `$(sha256)` and decompiled name-blind.",
    "kind:label sets must match. Empty detections are allowed.",
    "",
    f"- binaries: {len(rows)}",
    f"- identical: **{sum(1 for r in rows if r['identical'])}**",
    f"- failures: **{fail}**",
    "",
    "| Binary | identical | named | hashed |",
    "|--------|-----------|-------|--------|",
]
for r in rows:
    md.append(
        f"| `{r['name']}` | {str(r['identical']).lower()} | "
        f"{', '.join(r['named_labels']) or '(none)'} | "
        f"{', '.join(r['hashed_labels']) or '(none)'} |"
    )
md.append("")
(root / "results/b6-rename-guard.md").write_text("\n".join(md) + "\n", encoding="utf-8")
(root / "results/b6-rename-guard.json").write_text(
    json.dumps({"rows": rows, "failures": fail}, indent=2) + "\n", encoding="utf-8"
)
print(f"B6_{'OK' if fail == 0 else 'FAIL'} n={len(rows)} fail={fail}")
raise SystemExit(1 if fail else 0)
PY
