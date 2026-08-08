#!/usr/bin/env bash
# regenerate-retdec-support.sh — rebuild retdec-support signature DB (Phase 7.2).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/build/support-regen"
STAMP="$(date -u +%Y-%m-%d)"
SIG_SCRIPT="${ROOT}/scripts/retdec-signature-from-library-creator.py"

echo "==> Output staging: ${OUT}"
mkdir -p "${OUT}"

if [[ ! -f "${SIG_SCRIPT}" ]]; then
	echo "Missing ${SIG_SCRIPT}" >&2
	exit 1
fi

echo "==> Toolchain detection"
declare -A TOOLCHAINS=()
command -v gcc >/dev/null 2>&1 && TOOLCHAINS[gcc]=$(gcc --version | head -1)
command -v clang >/dev/null 2>&1 && TOOLCHAINS[clang]=$(clang --version | head -1)
command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 && TOOLCHAINS[mingw]=ok
if command -v cl >/dev/null 2>&1; then TOOLCHAINS[msvc]=ok; fi

if [[ ${#TOOLCHAINS[@]} -eq 0 ]]; then
	echo "No supported toolchain found (need gcc, clang, MSVC, or MinGW)" >&2
	exit 1
fi

printf '%s\n' "${!TOOLCHAINS[@]}" | while read -r k; do
	echo "  ${k}: ${TOOLCHAINS[$k]}"
done > "${OUT}/toolchains.txt"

echo "==> Signature creator help"
python3 "${SIG_SCRIPT}" --help || true

cat > "${OUT}/README.txt" <<EOF
retdec-support regeneration staged ${STAMP}.
Toolchains: $(tr '\n' ' ' < "${OUT}/toolchains.txt")
Next steps:
  1. Point signature creator at each runtime library (MSVC/GCC/Clang/MinGW).
  2. Package tarball and upload as GitHub release.
  3. Update cmake/deps.cmake SUPPORT_PKG_URL and SUPPORT_PKG_SHA256.
EOF

echo "Done. See ${OUT}/README.txt"
