#!/usr/bin/env bash
# find_python.sh — Resolve a working Python interpreter (Git Bash / Linux / macOS).
# Usage: PYTHON="$(bash scripts/find_python.sh)" or: source scripts/find_python.sh; resolve_python
set -euo pipefail

resolve_python() {
	local c p
	for c in python3 python py; do
		if command -v "$c" >/dev/null 2>&1; then
			if "$c" -c "import sys" >/dev/null 2>&1; then
				printf '%s\n' "$(command -v "$c")"
				return 0
			fi
		fi
	done
	for p in \
		"/c/Python314/python.exe" \
		"/c/Python312/python.exe" \
		"/c/Users/odinl/AppData/Local/Programs/Python/Python312/python.exe"; do
		if [[ -x "$p" ]] && "$p" -c "import sys" >/dev/null 2>&1; then
			printf '%s\n' "$p"
			return 0
		fi
	done
	return 1
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	resolve_python
fi
