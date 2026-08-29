#!/usr/bin/env bash
# CACHE-05 — cache-off vs cache-on decompile of one or more fixtures must match.
# Usage: bash scripts/ci/check_cache_differential.sh --decompiler PATH \
#          --binary PATH [--binary PATH ...] [--timeout SECONDS]
set -euo pipefail

DEC=""
TIMEOUT=180
BINS=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--binary) BINS+=("$2"); shift 2 ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${DEC}" || ! -x "${DEC}" ]]; then
	echo "retdec-decompiler not found" >&2
	exit 1
fi
if [[ ${#BINS[@]} -eq 0 ]]; then
	echo "fixture binary not found: <empty>" >&2
	exit 1
fi
for BIN in "${BINS[@]}"; do
	if [[ -z "${BIN}" || ! -f "${BIN}" ]]; then
		echo "fixture binary not found: ${BIN:-<empty>}" >&2
		exit 1
	fi
done

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

run_one() {
	local bin="$1"
	local cache="$2"
	local out="$3"
	rm -f "${out}" "${out%.c}.retdec-fn-cache.json"
	RETDEC_INCREMENTAL_CACHE="${cache}" timeout --kill-after=15 "${TIMEOUT}" \
		"${DEC}" -o "${out}" "${bin}" >/dev/null
	if [[ ! -s "${out}" ]]; then
		echo "empty output: ${out}" >&2
		exit 1
	fi
}

check_one() {
	local bin="$1"
	local stem
	stem="$(basename "${bin}")"
	local off="${WORK}/${stem}.off.c"
	local on1="${WORK}/${stem}.on1.c"
	local on2="${WORK}/${stem}.on2.c"

	run_one "${bin}" 0 "${off}"
	run_one "${bin}" 1 "${on1}"
	run_one "${bin}" 1 "${on2}"

	if ! cmp -s "${off}" "${on1}"; then
		echo "CACHE-05 FAIL: cache-off vs cache-on (first run) differ for ${bin}" >&2
		diff -u "${off}" "${on1}" | head -n 80 >&2 || true
		exit 1
	fi
	if ! cmp -s "${on1}" "${on2}"; then
		echo "CACHE-05 FAIL: cache-on first vs second run differ for ${bin}" >&2
		diff -u "${on1}" "${on2}" | head -n 80 >&2 || true
		exit 1
	fi

	echo "CACHE-05 OK: cache-off equals cache-on (byte-identical C) for ${bin}"
}

for BIN in "${BINS[@]}"; do
	check_one "${BIN}"
done
