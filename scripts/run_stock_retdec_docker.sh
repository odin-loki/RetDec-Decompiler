#!/usr/bin/env bash
# run_stock_retdec_docker.sh — Stock RetDec v5.0 via remnux/retdec (official Hub image
# retdec/retdec:v5.0 does not exist). Uses Docker Desktop; WSL docker CLI is optional.
# Usage: bash scripts/run_stock_retdec_docker.sh [--profile ci-core|full] [--skip-pull]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="ci-core"
SKIP_PULL=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--profile) PROFILE="$2"; shift 2 ;;
		--skip-pull) SKIP_PULL=(--skip-pull); shift ;;
		--tag|--image)
			echo "Image is remnux/retdec (stock v5.0). Override with DOCKER_STOCK_IMAGE if needed." >&2
			shift 2
			;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

PYTHON="${PYTHON:-python3}"
if ! command -v "${PYTHON}" >/dev/null 2>&1; then
	PYTHON="$("${ROOT}/scripts/find_python.sh")"
fi

IMAGE_ARGS=()
if [[ -n "${DOCKER_STOCK_IMAGE:-}" ]]; then
	IMAGE_ARGS=(--image "${DOCKER_STOCK_IMAGE}")
fi

exec "${PYTHON}" "${ROOT}/scripts/run_stock_retdec_docker.py" \
	--profile "${PROFILE}" \
	"${SKIP_PULL[@]}" \
	"${IMAGE_ARGS[@]}"
