#!/usr/bin/env bash
# run_stock_retdec_docker.sh — Run DecompileBench with stock RetDec v5.0 via Docker.
# Usage: bash scripts/run_stock_retdec_docker.sh [--profile ci-core|full] [--tag v5.0]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="ci-core"
TAG="v5.0"
IMAGE="retdec/retdec:${TAG}"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--profile) PROFILE="$2"; shift 2 ;;
		--tag) TAG="$2"; IMAGE="retdec/retdec:${TAG}"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if ! command -v docker >/dev/null 2>&1; then
	echo "docker not found — install Docker Desktop and enable WSL integration" >&2
	exit 1
fi

echo "==> Pulling ${IMAGE}"
docker pull "${IMAGE}"

bash "${ROOT}/scripts/fetch_decompilebench_corpus.sh" --profile "${PROFILE}"

LIMIT=()
[[ "${PROFILE}" == "ci-core" ]] && LIMIT=(--limit 9)

WORKDIR="${ROOT}/build/stock-docker-work"
rm -rf "${WORKDIR}"
mkdir -p "${WORKDIR}/corpus" "${WORKDIR}/out"

# Copy corpus binaries (resolve symlinks) into docker-visible dir.
python3 - "${ROOT}/tests/decompilebench/corpus/manifest.json" "${WORKDIR}/corpus" <<'PY'
import json, shutil, sys
from pathlib import Path
manifest, dest = Path(sys.argv[1]), Path(sys.argv[2])
for item in json.loads(manifest.read_text(encoding="utf-8")):
    src = Path(item["path"])
    if not src.is_file():
        src = Path(item.get("name", ""))
    if src.is_file():
        shutil.copy2(src, dest / Path(item["name"]).name)
PY

echo "==> Running stock RetDec in Docker"
docker run --rm \
	-v "${WORKDIR}/corpus:/corpus:ro" \
	-v "${WORKDIR}/out:/out" \
	"${IMAGE}" \
	bash -lc '
set -e
for f in /corpus/*; do
	[[ -f "$f" ]] || continue
	base=$(basename "$f")
	retdec-decompiler "$f" --output "/out/${base}.c" || true
done
'

OUT="${ROOT}/results/stock-retdec-docker-${PROFILE}.json"
python3 - "${ROOT}" "${WORKDIR}" "${OUT}" "${PROFILE}" <<'PY'
import json, subprocess, sys, time
from pathlib import Path

root, work, out, profile = map(Path, sys.argv[1:5])
corpus = work / "corpus"
rows = []
for sample in sorted(corpus.iterdir()):
    if not sample.is_file():
        continue
    c_out = work / "out" / f"{sample.name}.c"
    rows.append({
        "input": str(sample),
        "syntax_valid": c_out.is_file() and c_out.stat().st_size > 0,
        "output_c": str(c_out),
    })

payload = {
    "harness": "stock_retdec_docker",
    "image": "retdec/retdec",
    "profile": profile,
    "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "samples": rows,
    "summary": {
        "count": len(rows),
        "syntax_valid_rate": sum(1 for r in rows if r["syntax_valid"]) / len(rows) if rows else 0.0,
    },
}
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {out}")
PY

echo "Stock Docker benchmark complete: ${OUT}"
