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

SELF="${BASH_SOURCE[0]}"
ROOT="$(cd "$(dirname "${SELF}")/../.." && pwd)"

# --self-test: does this script actually fail when two runs disagree?  A gate
# that cannot fail is worse than no gate, so it is checked against decompilers
# that behave in the four ways that matter.
if [[ "${1:-}" == "--self-test" ]]; then
	set +e
	T="$(mktemp -d)"
	trap 'rm -rf "${T}"' EXIT
	mkdir -p "${T}/corpus" "${T}/empty" "${T}/bin"
	for n in prog_a-gcc-O0 prog_b-clang-O2 prog_c-gcc-O3; do
		printf 'fake %s\n' "${n}" > "${T}/corpus/${n}"
		chmod +x "${T}/corpus/${n}"
	done
	printf '{}' > "${T}/corpus/manifest.json"

	# Writes the same thing every time.
	cat > "${T}/bin/steady" <<'FAKE'
#!/usr/bin/env bash
out=""; bin=""
while [[ $# -gt 0 ]]; do case "$1" in -o) out="$2"; shift 2;; *) bin="$1"; shift;; esac; done
printf '// %s\n' "$(basename "${bin}")" > "${out}"
FAKE
	# Writes something different each run, for one binary out of three.
	cat > "${T}/bin/wobbly" <<'FAKE'
#!/usr/bin/env bash
out=""; bin=""
while [[ $# -gt 0 ]]; do case "$1" in -o) out="$2"; shift 2;; *) bin="$1"; shift;; esac; done
if [[ "$(basename "${bin}")" == "prog_b-clang-O2" ]]; then
	printf '// %s %s\n' "$(basename "${bin}")" "${RANDOM}${RANDOM}" > "${out}"
else
	printf '// %s\n' "$(basename "${bin}")" > "${out}"
fi
FAKE
	# Cannot decompile one of the three at all.
	cat > "${T}/bin/partial" <<'FAKE'
#!/usr/bin/env bash
out=""; bin=""
while [[ $# -gt 0 ]]; do case "$1" in -o) out="$2"; shift 2;; *) bin="$1"; shift;; esac; done
if [[ "$(basename "${bin}")" == "prog_c-gcc-O3" ]]; then
	echo "fake-decompiler: Assertion \`this cannot happen\' failed."
	exit 1
fi
printf '// %s\n' "$(basename "${bin}")" > "${out}"
FAKE
	# Cannot decompile anything.
	printf '#!/usr/bin/env bash\nexit 1\n' > "${T}/bin/dead"
	chmod +x "${T}/bin"/*

	fails=0
	expect() {
		local want="$1"; shift
		local desc="$1"; shift
		"$@" >"${T}/out" 2>&1
		local got=$?
		if [[ "${got}" -ne "${want}" ]]; then
			echo "self-test: ${desc}: expected exit ${want}, got ${got}" >&2
			cat "${T}/out" >&2
			fails=$(( fails + 1 ))
		fi
	}
	expect 0 "a steady decompiler passes" \
		bash "${SELF}" --decompiler "${T}/bin/steady" --corpus "${T}/corpus"
	expect 1 "a decompiler that wobbles on one binary fails" \
		bash "${SELF}" --decompiler "${T}/bin/wobbly" --corpus "${T}/corpus"
	expect 0 "a binary that does not decompile is skipped, not failed" \
		bash "${SELF}" --decompiler "${T}/bin/partial" --corpus "${T}/corpus"
	# ...and the run says why, rather than only that it happened.
	if ! grep -q "this cannot happen" "${T}/out"; then
		echo "self-test: a skipped binary did not report why it produced no output" >&2
		cat "${T}/out" >&2
		fails=$(( fails + 1 ))
	fi
	expect 1 "a decompiler that produces nothing at all fails" \
		bash "${SELF}" --decompiler "${T}/bin/dead" --corpus "${T}/corpus"
	expect 1 "an empty corpus fails" \
		bash "${SELF}" --decompiler "${T}/bin/steady" --corpus "${T}/empty"
	expect 0 "a limit smaller than the corpus is honoured" \
		bash "${SELF}" --decompiler "${T}/bin/steady" --corpus "${T}/corpus" --limit 2

	# The sample a limit takes has to be the same every time, or a green run
	# says nothing about what the next one will check.
	sample_once() {
		bash "${SELF}" --decompiler "${T}/bin/steady" --corpus "${T}/corpus" \
			--limit 2 --print-sample
	}
	if [[ "$(sample_once)" != "$(sample_once)" ]]; then
		echo "self-test: two --limit samples of the same corpus differ" >&2
		fails=$(( fails + 1 ))
	fi

	if [[ "${fails}" -ne 0 ]]; then
		echo "DET-01 self-test: ${fails} case(s) failed" >&2
		exit 1
	fi
	echo "DET-01 self-test OK"
	exit 0
fi

DEC=""
CORPUS=""
LIMIT=0
TIMEOUT=120
PRINT_SAMPLE=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--corpus) CORPUS="$2"; shift 2 ;;
		--limit) LIMIT="$2"; shift 2 ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		--print-sample) PRINT_SAMPLE=1; shift ;;
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

# A limit takes a sample ordered by a hash of the name rather than every N-th
# of the sorted list: the corpus is named <program>-<compiler>-O<level>, so it
# sorts into groups of six and every third entry is an -O0 one.  Hashing the
# name spreads the sample over compilers and optimisation levels without this
# script having to know how the names are built, and gives the same sample on
# every run.
BINS=()
if [[ "${LIMIT}" -gt 0 && ${#ALL[@]} -gt "${LIMIT}" ]]; then
	mapfile -t BINS < <(
		for b in "${ALL[@]}"; do
			printf '%s %s\n' "$(basename "${b}" | md5sum | cut -c1-16)" "${b}"
		done | sort | head -n "${LIMIT}" | cut -d' ' -f2-
	)
else
	BINS=("${ALL[@]}")
fi

if [[ "${PRINT_SAMPLE}" -eq 1 ]]; then
	printf '%s\n' "${BINS[@]}"
	exit 0
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
		# gate's business, not this one's -- but that gate only runs the nine
		# ci-core names, so for anything else this is the only place the
		# failure is visible.  Say why, using the same summariser the gate
		# uses, rather than only that it happened.
		echo "DET-01: skipping ${stem} -- no output on the first run"
		why="$(python3 -c '
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from extract_decompiler_predictions import summarise_failure_output
print(summarise_failure_output(Path(sys.argv[2]).read_text(errors="replace")))
' "${ROOT}/scripts" "${WORK}/${stem}.1.log" 2>/dev/null || true)"
		[[ -n "${why}" ]] && echo "DET-01:   ${stem}: ${why}"
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
