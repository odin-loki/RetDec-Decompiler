#!/usr/bin/env bash
# SEM-01 — build and run tests/sem_decoder against the system Capstone.
#
# Why this exists
# ---------------
# `tests/sem_decoder/sem_decoder_test.cpp` was run by nothing.
#
#   * `scripts/standalone_check.sh` builds only the modules that need no
#     third-party library, and src/sem_decoder includes <capstone/capstone.h>.
#     It is listed in that script's EXCLUDED_REASONS, which keeps the module out
#     of the fast path -- and silently takes its 45 test cases with it, because
#     the --audit suite check only fires for suites whose module IS in the fast
#     path.
#   * ctest-linux does configure with RETDEC_TESTS=ON (full-linux-debug sets it,
#     overriding _optionDefaults), so `retdec-sem-decoder-tests` is a configured
#     target -- but the build step names its targets explicitly and this is not
#     one of them, and `gtest_discover_tests` attaches NO labels, so the
#     `ctest -L unit` step does not select it either. Measured with a two-test
#     probe project: an unbuilt discovered target shows as
#     `<target>_NOT_BUILT` under plain `ctest -N` and is absent under
#     `ctest -L unit -N`; a BUILT one is still absent under `-L unit`.
#
# So the decoder that turns machine code into semantic IR -- and the DP that
# picks which of the overlapping decodes is real -- had no gate at all. Two
# defects were sitting in it: bestPath relaxed no edge (operator[] inserted a
# 0.0 before the -infinity guard could run, and every score is negative), and
# propagateUndefFlags cleared the preserved mask, which reduced its transfer
# function to "whatever this instruction says", so undefinedness never crossed
# an instruction boundary.
#
# What it does
# ------------
# Compiles src/sem_decoder and its suite against whatever Capstone the runner
# provides, with the repository's own lite gtest, and runs them. That is a
# different Capstone from the pinned one the full build downloads, and
# deliberately so: the API this module touches -- cs_open, cs_disasm,
# cs_detail.x86 -- is the stable part, and a check that runs on a plain
# developer machine is worth more than one that needs a two-hour dependency
# build. If the module ever reaches for something version-specific, this stops
# compiling and says so, which is itself the signal.
#
# It fails, rather than skipping, when no Capstone is found. A check that
# quietly turns itself off is a check nobody can rely on.
#
# Usage: bash scripts/ci/check_sem_decoder_tests.sh [--min-tests N] [--keep]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

MIN_TESTS=45
KEEP=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--min-tests) MIN_TESTS="$2"; shift 2 ;;
		--keep) KEEP=1; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 2 ;;
	esac
done

CXX_BIN="${CXX:-g++}"

# pkg-config first: it knows about a Capstone installed anywhere. Fall back to
# a bare -lcapstone, which covers a distribution package in the default paths.
CAPSTONE_CFLAGS=()
CAPSTONE_LIBS=(-lcapstone)
CAPSTONE_VERSION="unknown"
if pkg-config --exists capstone 2>/dev/null; then
	mapfile -t CAPSTONE_CFLAGS < <(pkg-config --cflags capstone | tr ' ' '\n' | grep -v '^$' || true)
	mapfile -t CAPSTONE_LIBS   < <(pkg-config --libs   capstone | tr ' ' '\n' | grep -v '^$' || true)
	CAPSTONE_VERSION="$(pkg-config --modversion capstone)"
fi

WORK="$(mktemp -d)"
if [[ "${KEEP}" -eq 0 ]]; then
	trap 'rm -rf "${WORK}"' EXIT
else
	echo "SEM-01: keeping ${WORK}"
fi

# Probe before building the real thing, so "no Capstone" reports itself as that
# rather than as a wall of missing-header errors from the module.
printf '#include <capstone/capstone.h>\nint main(void){ return cs_version(0,0) ? 0 : 1; }\n' \
	> "${WORK}/probe.c"
if ! "${CXX_BIN}" -x c++ "${WORK}/probe.c" "${CAPSTONE_CFLAGS[@]}" \
	"${CAPSTONE_LIBS[@]}" -o "${WORK}/probe" > "${WORK}/probe.log" 2>&1
then
	echo "SEM-01: no usable Capstone found." >&2
	echo "SEM-01: this check builds src/sem_decoder, which needs the Capstone" >&2
	echo "        headers and library. Install them (Debian/Ubuntu:" >&2
	echo "        'apt-get install libcapstone-dev')." >&2
	tail -n 10 "${WORK}/probe.log" >&2
	exit 1
fi

echo "SEM-01: building against Capstone ${CAPSTONE_VERSION}"

SRCS=(
	src/sem_decoder/sem_decoder.cpp
	tests/sem_decoder/sem_decoder_test.cpp
	tests/standalone/gtest_lite.cpp
	tests/standalone/gtest_lite_main.cpp
)

BIN="${WORK}/sem_decoder_test"
if ! "${CXX_BIN}" -std=c++17 -O0 -g \
	-Iinclude -Itests/standalone "${CAPSTONE_CFLAGS[@]}" \
	"${SRCS[@]}" -o "${BIN}" "${CAPSTONE_LIBS[@]}" \
	> "${WORK}/build.log" 2>&1
then
	echo "SEM-01: FAIL sem_decoder did not build against Capstone ${CAPSTONE_VERSION}" >&2
	grep -E 'error:' "${WORK}/build.log" | head -n 20 >&2 || tail -n 20 "${WORK}/build.log" >&2
	exit 1
fi

if ! "${BIN}" > "${WORK}/run.log" 2>&1; then
	echo "SEM-01: FAIL the sem_decoder tests do not pass" >&2
	tail -n 40 "${WORK}/run.log" >&2
	exit 1
fi

# The count is printed so a suite that silently stops registering tests is
# visible; "0 tests ran" passes every assertion it has. --min-tests is the
# floor: it is what stops a suite from shrinking without anyone noticing.
RAN="$(grep -aoE '[0-9]+ tests? ran' "${WORK}/run.log" | tail -n1 || true)"
if [[ -z "${RAN}" ]]; then
	echo "SEM-01: FAIL could not tell how many tests ran" >&2
	tail -n 20 "${WORK}/run.log" >&2
	exit 1
fi
COUNT="${RAN%% *}"
if [[ "${COUNT}" -lt "${MIN_TESTS}" ]]; then
	echo "SEM-01: FAIL only ${COUNT} tests ran, expected at least ${MIN_TESTS}" >&2
	echo "        If the suite shrank on purpose, lower --min-tests and say why." >&2
	exit 1
fi

echo "SEM-01: OK ${RAN} against Capstone ${CAPSTONE_VERSION}"
