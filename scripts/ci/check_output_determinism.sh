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
	# Crashes the way the real one does: an assertion, then an LLVM backtrace
	# printed innermost frame first, then the report banner.  Frame #0 and the
	# assertion are forty-odd lines from the end of that, which is the whole
	# point of the case -- a check that printed the tail of the log would show
	# main() and miss both.
	cat > "${T}/bin/crasher" <<'FAKE'
#!/usr/bin/env bash
out=""; bin=""
while [[ $# -gt 0 ]]; do case "$1" in -o) out="$2"; shift 2;; *) bin="$1"; shift;; esac; done
if [[ "$(basename "${bin}")" != "prog_c-gcc-O3" ]]; then
	printf '// %s\n' "$(basename "${bin}")" > "${out}"
	exit 0
fi
for i in $(seq 1 60); do echo "Running phase: filler ${i} ( 0.01s )"; done
echo "fake-decompiler: /src/llvmir2hll/evaluator.cpp:91: void Ev::visit(): Assertion \`operandIsSupported\' failed."
echo " #0 0x0000000000000000 llvm::sys::PrintStackTrace(llvm::raw_ostream&, int)"
echo " #1 0x0000000000000001 TheFunctionThatCrashed(retdec::llvmir2hll::Module*)"
for i in $(seq 2 40); do echo " #${i} 0x00000000000000${i} some_frame_${i}()"; done
echo "PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/ and include the crash backtrace."
echo "Stack dump:"
printf '0.\tProgram arguments: fake-decompiler\n'
printf "1.\tRunning pass 'LLVM IR -> HLL' on module ''.\n"
exit 134
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
	expect 0 "a decompiler that crashes on one binary is skipped, not failed" \
		bash "${SELF}" --decompiler "${T}/bin/crasher" --corpus "${T}/corpus"
	# ...and the excerpt reaches the top of the stack, not the bottom of the
	# log.  Both of these are forty-odd lines from the end.
	for want in "TheFunctionThatCrashed" "operandIsSupported"; do
		if ! grep -q "${want}" "${T}/out"; then
			echo "self-test: a crashing binary's report did not include ${want}" >&2
			cat "${T}/out" >&2
			fails=$(( fails + 1 ))
		fi
	done

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

# LLVM's crash handler symbolizes its own backtrace only when it can find an
# llvm-symbolizer -- through $LLVM_SYMBOLIZER_PATH, next to argv[0], or on
# $PATH.  With none of those it falls back to dladdr, which resolves only
# exported symbols, and this binary exports none of its own.  That is why the
# frames printed below used to read
#
#   16 retdec-decompiler 0x00005643ba734e06
#
# for every frame in the decompiler and name nothing.  Finding a symbolizer
# here turns the same frames into function, file and line.
find_symbolizer() {
	local p
	if [[ -n "${LLVM_SYMBOLIZER_PATH:-}" && -x "${LLVM_SYMBOLIZER_PATH}" ]]; then
		printf '%s\n' "${LLVM_SYMBOLIZER_PATH}"
		return 0
	fi
	if p="$(command -v llvm-symbolizer 2>/dev/null)"; then
		printf '%s\n' "${p}"
		return 0
	fi
	# Distributions ship it under a versioned prefix and only add the
	# unversioned name with the `llvm` metapackage; newest version first.
	for p in $(ls -d /usr/lib/llvm-*/bin/llvm-symbolizer /usr/bin/llvm-symbolizer-* \
		2>/dev/null | sort -Vr); do
		if [[ -x "${p}" ]]; then
			printf '%s\n' "${p}"
			return 0
		fi
	done
	return 1
}

if SYMBOLIZER="$(find_symbolizer)"; then
	export LLVM_SYMBOLIZER_PATH="${SYMBOLIZER}"
else
	SYMBOLIZER=""
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# The analysis-cache sidecar is derived from the output .c path, so the two
# runs write to different paths and neither can see a cache the other left.
# RETDEC_INCREMENTAL_CACHE=0 turns it off in any case; this is about the
# decompiler, not about the cache.
# The third argument is a padding length. The two runs of a binary get
# different ones, which changes the size of the environment block and with it
# where the kernel puts the stack -- and, through that, the addresses the
# allocator hands out.
#
# That matters because the defects this check exists to catch are almost all
# pointer-order dependence: an unordered_set keyed by shared_ptr iterates in
# hash order, which is address order. Two runs with the same layout can agree
# for a long time and then disagree once. Making the layouts differ on purpose
# turns "sometimes" into "usually" -- the node-splitting defect DET-01 first
# caught on mergesort-gcc-O3 had passed the two runs before it.
run_one() {
	local bin="$1"
	local out="$2"
	local padlen="${3:-0}"
	local log="${out%.c}.log"
	local pad=""
	[[ "${padlen}" -gt 0 ]] && printf -v pad '%*s' "${padlen}" ''
	rm -f "${out}" "${log}" "${out%.c}.retdec-fn-cache.json"
	RETDEC_INCREMENTAL_CACHE=0 RETDEC_DET_LAYOUT_PAD="${pad}" \
		timeout --kill-after=15 "${TIMEOUT}" \
		"${DEC}" -o "${out}" "${bin}" >"${log}" 2>&1 || true
	[[ -s "${out}" ]]
}

# How much of a crashing run's log to print.  Bounded, because three crashing
# binaries times an unbounded log is a CI page nobody reads.
CRASH_CONTEXT_LINES=80

# LLVM prints a backtrace innermost frame first, so the *end* of a crash log is
# main() and __libc_start_main and the last twenty-five lines of it name
# nothing.  That is what this used to print: twenty frames counted from the
# bottom of the stack, none of them the ones that say where the crash was.
#
# The report banner sits just after the last frame, so anchoring on it and
# printing the window above it gets frame #0 -- and any assertion message,
# which glibc writes before the handler runs and which is the single most
# useful line in the file.
crash_excerpt() {
	local log="$1"
	local marker start
	# `|| true`: no banner is the ordinary case for a plain non-zero exit, and
	# under `set -o pipefail` grep's failure would otherwise end the run.
	marker="$( { grep -n -m1 -e 'PLEASE submit a bug report' -e '^Stack dump:' \
		"${log}" || true; } | head -n1 | cut -d: -f1)"
	if [[ -z "${marker}" ]]; then
		tail -n "${CRASH_CONTEXT_LINES}" "${log}"
		return 0
	fi
	start=$(( marker - CRASH_CONTEXT_LINES ))
	[[ "${start}" -lt 1 ]] && start=1
	sed -n "${start},\$p" "${log}"
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
		# The summariser keeps a few lines from each end, which for an LLVM
		# crash is the pass name but not the frames -- and the frames are what
		# name the code. Print them verbatim as well.
		if [[ -s "${WORK}/${stem}.1.log" ]]; then
			echo "DET-01:   --- crash excerpt from ${stem}${SYMBOLIZER:+ (symbolized by ${SYMBOLIZER})} ---"
			crash_excerpt "${WORK}/${stem}.1.log" | sed "s/^/DET-01:   /"
		fi
		skipped=$(( skipped + 1 ))
		continue
	fi
	if ! run_one "${bin}" "${second}" 4096; then
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
