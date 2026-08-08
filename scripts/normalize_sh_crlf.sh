#!/usr/bin/env bash
# normalize_sh_crlf.sh — Strip CRLF from scripts/*.sh (Windows editor safety).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
find "${ROOT}/scripts" -name '*.sh' -exec sed -i 's/\r$//' {} +
echo "Normalized CRLF in scripts/*.sh"
