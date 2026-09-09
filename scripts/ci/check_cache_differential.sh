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

# The analysis-cache sidecar is derived from the output .c path
# (functionAnalysisCachePath in src/retdec/function_analysis_cache.cpp), so a
# run only sees a cache another run left if the two write to the SAME output
# path. The two cache-on runs used to write to on1.c and on2.c and this
# function deleted the sidecar before each of them, so all three runs below
# were cold: cacheHits was 0 every time and the check compared a cache-off run
# against two more cache-off runs. It would have passed for a decompiler whose
# cache was never read.
#
# keep_cache=1 leaves the sidecar in place, which is what makes the third run
# warm.
run_one() {
	local bin="$1"
	local cache="$2"
	local out="$3"
	local keep_cache="${4:-0}"
	local log="${out%.c}.log"
	rm -f "${out}" "${log}"
	[[ "${keep_cache}" -eq 1 ]] || rm -f "${out%.c}.retdec-fn-cache.json"
	RETDEC_INCREMENTAL_CACHE="${cache}" timeout --kill-after=15 "${TIMEOUT}" \
		"${DEC}" -o "${out}" "${bin}" >"${log}" 2>&1
	if [[ ! -s "${out}" ]]; then
		echo "empty output: ${out}" >&2
		cat "${log}" >&2 || true
		exit 1
	fi
}

# The decompiler prints "[analysis] function cache: N hit(s), M miss(es)" only
# when N is above zero, so its absence is the signal that nothing was reused.
cache_hits() {
	local log="$1"
	sed -n 's/.*function cache: \([0-9]\+\) hit(s).*/\1/p' "${log}" | head -1
}

check_one() {
	local bin="$1"
	local stem
	stem="$(basename "${bin}")"
	local off="${WORK}/${stem}.off.c"
	local on="${WORK}/${stem}.on.c"
	local cold="${WORK}/${stem}.cold.c"
	local sidecar="${WORK}/${stem}.on.retdec-fn-cache.json"

	run_one "${bin}" 0 "${off}"

	# Two cache-off runs first. If these two differ, the decompiler's own
	# output is not reproducible and nothing below tells you anything about the
	# cache -- which is a different bug report, and a more serious one. Say so
	# in those words rather than blaming the cache for it.
	local off2="${WORK}/${stem}.off2.c"
	run_one "${bin}" 0 "${off2}"
	if ! cmp -s "${off}" "${off2}"; then
		echo "CACHE-05 FAIL: two cache-off runs of ${bin} differ -- this is decompiler nondeterminism, not a cache defect" >&2
		diff -u "${off}" "${off2}" | head -n 80 >&2 || true
		exit 1
	fi

	# Cold with the cache on: writes the sidecar.
	run_one "${bin}" 1 "${on}"
	cp "${on}" "${cold}"
	if [[ ! -s "${sidecar}" ]]; then
		echo "CACHE-05 FAIL: no analysis cache written for ${bin} (expected ${sidecar})" >&2
		exit 1
	fi

	# Warm: same output path, sidecar kept, so this run reads what the last
	# one wrote.
	run_one "${bin}" 1 "${on}" 1
	local hits
	hits="$(cache_hits "${WORK}/${stem}.on.log")"
	if [[ -z "${hits}" || "${hits}" -le 0 ]]; then
		echo "CACHE-05 FAIL: warm run reused nothing for ${bin} -- the cache is not being read" >&2
		exit 1
	fi

	if ! cmp -s "${off}" "${cold}"; then
		echo "CACHE-05 FAIL: cache-off vs cache-on (cold run) differ for ${bin}" >&2
		diff -u "${off}" "${cold}" | head -n 80 >&2 || true
		exit 1
	fi
	if ! cmp -s "${cold}" "${on}"; then
		echo "CACHE-05 FAIL: cold vs warm cache-on run differ for ${bin}" >&2
		diff -u "${cold}" "${on}" | head -n 80 >&2 || true
		exit 1
	fi

	echo "CACHE-05 OK: cache-off equals cache-on for ${bin} (warm run reused ${hits} function(s))"
}

for BIN in "${BINS[@]}"; do
	check_one "${BIN}"
done
