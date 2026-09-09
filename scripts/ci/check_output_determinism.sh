#!/usr/bin/env bash
# DET-01 — two runs of the decompiler on the same binary must produce the same C.
#
# CACHE-05 already compares two cache-off runs, but only of the nine ci-core
# names, all of them gcc -O0.  Nondeterminism hides in the shapes those do not
# have: more functions (the HLL copy-propagation passes only go parallel above
# twenty-four), denser CFGs, a different compiler's idioms.  This sweeps the
# whole built corpus instead, two runs each, and says nothing about caching.
#
# Usage: bash scripts/ci/check_output_determinism.sh --decompiler PATH \
#          --corpus DIR [--limit N] [--timeout SECONDS]
set -euo pipefail

DEC=""
CORPUS=""
LIMIT=0
TIMEOUT=120
while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--corpus) CORPUS="$2"; shift 2 ;;
		--limit) LIMIT="$2"; shift 2 ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${DEC}" || ! -x "${DEC}" ]]; then
	echo "retdec-decompiler not found" >&2
	exit 1
fi
if [[ -z "${CORPUS}" || ! -d "${CORPUS}" ]]; then
	echo "corpus directory not found: ${CORPUS:-<empty>}" >&2
	exit 1
fi

# Sorted, so the selection below is the same on every run.
mapfile -t ALL < <(find "${CORPUS}" -maxdepth 1 -type f -perm -u+x ! -name '*.json' \
	! -name '*.c' ! -name '*.txt' -print | sort)

if [[ ${#ALL[@]} -eq 0 ]]; then
	echo "DET-01: no binaries under ${CORPUS} -- nothing to check" >&2
	exit 1
fi

# An even spread rather than the first N, so that a limit still covers every
# compiler and optimisation level the corpus was built with.
BINS=()
if [[ "${LIMIT}" -gt 0 && ${#ALL[@]} -gt "${LIMIT}" ]]; then
	step=$(( (${#ALL[@]} + LIMIT - 1) / LIMIT ))
	for (( i = 0; i < ${#ALL[@]}; i += step )); do
		BINS+=("${ALL[$i]}")
	done
else
	BINS=("${ALL[@]}")
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# The analysis-cache sidecar is derived from the output .c path, so the two
# runs write to different paths and neither can see a cache the other left.
# RETDEC_INCREMENTAL_CACHE=0 turns it off in any case; this is about the
# decompiler, not about the cache.
run_one() {
	local bin="$1"
	local out="$2"
	local log="${out%.c}.log"
	rm -f "${out}" "${log}" "${out%.c}.retdec-fn-cache.json"
	RETDEC_INCREMENTAL_CACHE=0 timeout --kill-after=15 "${TIMEOUT}" \
		"${DEC}" -o "${out}" "${bin}" >"${log}" 2>&1 || true
	[[ -s "${out}" ]]
}

checked=0
skipped=0
for bin in "${BINS[@]}"; do
	stem="$(basename "${bin}")"
	first="${WORK}/${stem}.1.c"
	second="${WORK}/${stem}.2.c"

	if ! run_one "${bin}" "${first}"; then
		# A binary this build cannot decompile at all is the algorithm-recovery
		# gate's business, not this one's.
		echo "DET-01: skipping ${stem} -- no output on the first run"
		skipped=$(( skipped + 1 ))
		continue
	fi
	if ! run_one "${bin}" "${second}"; then
		echo "DET-01 FAIL: ${stem} produced output on the first run and none on the second" >&2
		cat "${WORK}/${stem}.2.log" >&2 || true
		exit 1
	fi

	if ! cmp -s "${first}" "${second}"; then
		echo "DET-01 FAIL: two runs of ${bin} produced different C" >&2
		diff -u "${first}" "${second}" | head -n 80 >&2 || true
		exit 1
	fi
	checked=$(( checked + 1 ))
done

if [[ "${checked}" -eq 0 ]]; then
	echo "DET-01 FAIL: nothing was checked (${skipped} binary/binaries produced no output)" >&2
	exit 1
fi

echo "DET-01 OK: ${checked} binary/binaries decompiled identically twice (${skipped} skipped, ${#ALL[@]} in the corpus)"
