#!/usr/bin/env bash
# upgrade-dep.sh — Bump one dependency pin in cmake/deps.cmake, rebuild, test.
# Usage: bash scripts/upgrade-dep.sh <NAME> <URL> [SHA256]
# Example: bash scripts/upgrade-dep.sh CAPSTONE https://github.com/.../5.0.9.zip <sha>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="${ROOT}/cmake/deps.cmake"
NAME="${1:?dependency prefix e.g. CAPSTONE}"
URL="${2:?archive URL}"
SHA="${3:-}"

if [[ -z "${SHA}" ]]; then
	echo "Computing SHA-256 for ${URL}..."
	SHA="$(curl -fsSL "${URL}" | sha256sum | awk '{print $1}')"
fi

python3 - "${DEPS}" "${NAME}" "${URL}" "${SHA}" <<'PY'
import pathlib, re, sys
path, name, url, sha = sys.argv[1:5]
text = pathlib.Path(path).read_text(encoding="utf-8")
text = re.sub(rf'(set\({name}_URL\s*\n\s*")[^"]+(")', rf'\1{url}\2', text, count=1)
text = re.sub(rf'(set\({name}_ARCHIVE_SHA256\s*\n\s*")[^"]+(")', rf'\1{sha}\2', text, count=1)
pathlib.Path(path).write_text(text, encoding="utf-8")
print(f"Updated {name}_URL and {name}_ARCHIVE_SHA256")
PY

PRESET="${RETDEC_CMAKE_PRESET:-full-linux-debug}"
BUILD="${ROOT}/build/linux"
cmake --preset "${PRESET}" -DRETDEC_ENABLE_CUDA_ACCEL=OFF -DRETDEC_ENABLE_NEURAL=ON
cmake --build "${BUILD}" --parallel
ctest --test-dir "${BUILD}" --output-on-failure -L unit

if [[ -x "${ROOT}/scripts/run_benchmarks.sh" ]]; then
	"${ROOT}/scripts/run_benchmarks.sh" --compare baseline-2026-08 || true
fi

echo "Dependency ${NAME} bump complete."
