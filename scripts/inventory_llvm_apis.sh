#!/usr/bin/env bash
# inventory_llvm_apis.sh — LLVM 8 → next migration inventory (step 33).
# Usage: bash scripts/inventory_llvm_apis.sh [--out FILE]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/results/llvm-api-inventory.json"
PYTHON="$("${ROOT}/scripts/find_python.sh")"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--out) OUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "$(dirname "${OUT}")"

"${PYTHON}" - "${ROOT}" "${OUT}" <<'PY'
import json, pathlib, re, sys

root = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])

# Known LLVM 8 → modern breaking patterns (inventory only).
PATTERNS = {
    "getPointerElementType": r"getPointerElementType\s*\(",
    "CreateLoad_no_ty": r"CreateLoad\s*\([^,)]+\)",
    "getType()->getPointerElementType": r"getType\(\)->getPointerElementType",
    "legacy_pass_manager": r"llvm::PassManager\b",
    "IRBuilder_default": r"IRBuilder<>\s*\(",
}

hits: dict[str, list[dict]] = {k: [] for k in PATTERNS}
file_counts: dict[str, int] = {}
total_refs = 0

for path in sorted(root.rglob("*")):
    if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
        continue
    if "build-" in str(path) or "/external/" in str(path).replace("\\", "/"):
        continue
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        continue
    llvm_refs = len(re.findall(r"\bllvm::", text))
    if llvm_refs:
        rel = str(path.relative_to(root))
        file_counts[rel] = llvm_refs
        total_refs += llvm_refs
    for name, pat in PATTERNS.items():
        for m in re.finditer(pat, text):
            line = text.count("\n", 0, m.start()) + 1
            hits[name].append({"file": str(path.relative_to(root)), "line": line})

payload = {
    "harness": "llvm-api-inventory",
    "total_llvm_refs": total_refs,
    "files_with_llvm": len(file_counts),
    "breaking_patterns": {k: {"count": len(v), "samples": v[:5]} for k, v in hits.items()},
    "top_files": sorted(file_counts.items(), key=lambda x: -x[1])[:20],
    "next": "Address patterns before RETDEC_LLVM_NEXT bump; see docs/internal/retypd_sailr_llvm.md",
}
out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {out} ({total_refs} llvm:: refs in {len(file_counts)} files)")
PY
