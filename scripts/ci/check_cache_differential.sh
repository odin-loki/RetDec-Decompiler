#!/usr/bin/env bash
# CACHE-05 — cache-off vs cache-on decompile of one fixture must match.
# Usage: bash scripts/ci/check_cache_differential.sh --decompiler PATH --binary PATH
set -euo pipefail

DEC=""
BIN=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--binary) BIN="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${DEC}" || ! -x "${DEC}" ]]; then
	echo "retdec-decompiler not found" >&2
	exit 1
fi
if [[ -z "${BIN}" || ! -f "${BIN}" ]]; then
	echo "fixture binary not found: ${BIN:-<empty>}" >&2
	exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

run_one() {
	local cache="$1"
	local out="$2"
	rm -f "${out}" "${out%.c}.retdec-fn-cache.json"
	RETDEC_INCREMENTAL_CACHE="${cache}" timeout --kill-after=15 180 \
		"${DEC}" -o "${out}" "${BIN}" >/dev/null
	if [[ ! -s "${out}" ]]; then
		echo "empty output: ${out}" >&2
		exit 1
	fi
}

run_one 0 "${WORK}/off.c"
run_one 1 "${WORK}/on1.c"
run_one 1 "${WORK}/on2.c"

if ! cmp -s "${WORK}/off.c" "${WORK}/on1.c"; then
	echo "CACHE-05 FAIL: cache-off vs cache-on (first run) differ" >&2
	diff -u "${WORK}/off.c" "${WORK}/on1.c" | head -n 80 >&2 || true
	exit 1
fi
if ! cmp -s "${WORK}/on1.c" "${WORK}/on2.c"; then
	echo "CACHE-05 FAIL: cache-on first vs second run differ" >&2
	diff -u "${WORK}/on1.c" "${WORK}/on2.c" | head -n 80 >&2 || true
	exit 1
fi

echo "CACHE-05 OK: cache-off equals cache-on (byte-identical C) for ${BIN}"
