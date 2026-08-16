#!/usr/bin/env bash
# fetch_decompilebench_corpus.sh — Stage a runnable DecompileBench corpus for local/CI runs.
#
# Full arXiv 2505.11340 (OSS-Fuzz, 23k functions) needs Docker + oss-fuzz; see
# tests/decompilebench/README.md. Until that is wired, we use the algorithm-recovery
# corpus as a reproducible stand-in with known sources for coverage checks.
#
# Usage: bash scripts/fetch_decompilebench_corpus.sh [--profile ci-core|full]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="ci-core"
AR_CORPUS="${ROOT}/tests/algorithm_recovery/corpus"
AR_SRC="${ROOT}/tests/algorithm_recovery/sources"
OUT="${ROOT}/tests/decompilebench/corpus"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--profile) PROFILE="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ ! -f "${AR_CORPUS}/manifest.json" ]]; then
	echo "Building algorithm-recovery corpus first..."
	bash "${ROOT}/scripts/build_algorithm_corpus.sh" || true
fi
if [[ ! -f "${AR_CORPUS}/manifest.json" ]]; then
	echo "No algorithm-recovery corpus; skip DecompileBench staging."
	exit 0
fi

python3 - "${ROOT}" "${PROFILE}" "${AR_CORPUS}" "${AR_SRC}" "${OUT}" <<'PY'
import json, os, sys
from pathlib import Path

root = Path(sys.argv[1])
profile = sys.argv[2]
ar_corpus = Path(sys.argv[3])
ar_src = Path(sys.argv[4])
out = Path(sys.argv[5])
manifest = json.loads((ar_corpus / "manifest.json").read_text(encoding="utf-8"))

CI_CORE = {
    "bubblesort-gcc-O0",
    "mergesort-gcc-O0",
    "hash_table-gcc-O0",
    "ring_buffer-gcc-O0",
    "binary_search-gcc-O0",
    "memcpy_loop-gcc-O0",
    "generated_quicksort-gcc-O0",
    "generated_heapsort-gcc-O0",
    "generated_pthread_mutex-gcc-O0",
}

if profile == "ci-core":
    items = [m for m in manifest if m.get("name") in CI_CORE]
elif profile == "full":
    items = list(manifest)
else:
    raise SystemExit(f"Unknown profile: {profile}")

out.mkdir(parents=True, exist_ok=True)
# Clean stale symlinks/files except README
for p in out.iterdir():
    if p.name in ("README", "README.md", ".gitkeep"):
        continue
    if p.is_symlink() or p.is_file():
        p.unlink()

staged = []
for item in items:
    name = item["name"]
    src_bin = ar_corpus / name
    if not src_bin.is_file():
        alt = ar_corpus / f"{name}.exe"
        if alt.is_file():
            src_bin = alt
        else:
            continue
    dest = out / name
    if dest.exists():
        dest.unlink()
    os.symlink(src_bin.resolve(), dest)
    source_rel = item.get("source", "")
    source_path = str((ar_src / source_rel).resolve()) if source_rel else None
    staged.append({
        "name": name,
        "source": source_rel,
        "source_path": source_path,
        "compiler": item.get("compiler"),
        "opt": item.get("opt"),
        "path": str(dest.resolve()),
    })

(out / "manifest.json").write_text(json.dumps(staged, indent=2) + "\n", encoding="utf-8")
meta = {
    "profile": profile,
    "count": len(staged),
    "note": "Stand-in corpus from algorithm-recovery; not OSS-Fuzz DecompileBench.",
    "upstream_paper": "https://arxiv.org/abs/2505.11340",
}
(out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
print(f"Staged {len(staged)} binaries in {out} (profile={profile})")
PY
