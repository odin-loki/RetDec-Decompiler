#!/usr/bin/env bash
# dispatch_algorithm_recovery_nightly.sh — Trigger GitHub nightly workflow (requires gh auth).
# Usage: bash scripts/dispatch_algorithm_recovery_nightly.sh [--full-corpus]
set -euo pipefail

FULL=false
while [[ $# -gt 0 ]]; do
	case "$1" in
		--full-corpus) FULL=true; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if ! command -v gh >/dev/null 2>&1; then
	ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
	if [[ -x "${ROOT}/scripts/find_gh.sh" ]]; then
		GH="$("${ROOT}/scripts/find_gh.sh" 2>/dev/null || true)"
		if [[ -n "${GH}" ]]; then
			gh() { "${GH}" "$@"; }
		fi
	fi
fi

if ! command -v gh >/dev/null 2>&1; then
	echo "gh CLI not found — install from https://cli.github.com/" >&2
	exit 1
fi

if ! gh auth status >/dev/null 2>&1; then
	echo "Not authenticated — run: gh auth login" >&2
	exit 1
fi

ARGS=()
if [[ "${FULL}" == true ]]; then
	ARGS+=(-f "full_corpus=true")
	echo "Dispatching algorithm-recovery-nightly (full 216-binary corpus)..."
else
	echo "Dispatching algorithm-recovery-nightly (CI core subset)..."
fi

gh workflow run algorithm-recovery-nightly "${ARGS[@]}"
echo "Workflow dispatched. Monitor with: gh run list --workflow=algorithm-recovery-nightly"
