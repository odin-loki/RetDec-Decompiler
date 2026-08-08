#!/usr/bin/env bash
# find_gh.sh — Resolve gh CLI (native Linux or GitHub CLI for Windows via WSL).
set -euo pipefail

if command -v gh >/dev/null 2>&1; then
	command -v gh
	exit 0
fi

for candidate in \
	"/mnt/c/Program Files/GitHub CLI/gh.exe" \
	"/mnt/c/Program Files (x86)/GitHub CLI/gh.exe"; do
	if [[ -x "${candidate}" ]]; then
		echo "${candidate}"
		exit 0
	fi
done

exit 1
