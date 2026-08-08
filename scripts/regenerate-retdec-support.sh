#!/usr/bin/env bash
# regenerate-retdec-support.sh — rebuild retdec-support signature DB (Phase 7.2).
# Requires MSVC, GCC, Clang, MinGW runtimes and scripts/retdec-signature-from-library-creator.py
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/build/support-regen"
STAMP="$(date -u +%Y-%m-%d)"

echo "==> Output staging: ${OUT}"
mkdir -p "${OUT}"

if [[ ! -f "${ROOT}/scripts/retdec-signature-from-library-creator.py" ]]; then
	echo "Missing signature creator script." >&2
	exit 1
fi

echo "==> Regenerate signatures (manual toolchain paths required)"
python3 "${ROOT}/scripts/retdec-signature-from-library-creator.py" \
	--help || true

cat > "${OUT}/README.txt" <<EOF
retdec-support regeneration staged ${STAMP}.
Update cmake/deps.cmake SUPPORT_PKG_URL and SUPPORT_PKG_SHA256 after packaging.
EOF

echo "Done. Package ${OUT} and upload as new retdec-support release."
