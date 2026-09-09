#!/usr/bin/env bash
# ADAPT-01 — build and run the llvm_to_ssa tests against the system LLVM.
#
# Why this exists
# ---------------
# `tests/retdec/llvm_to_ssa_test.cpp` was run by nothing.  `scripts/
# standalone_check.sh` builds only the modules that need no LLVM, and this
# adapter needs `llvm/IR`; the CMake suite that would build it is behind
# `RETDEC_ENABLE_LLVMIR2HLL_TESTS`, which is behind `RETDEC_TESTS`, and
# ctest-linux configures with `RETDEC_TESTS:BOOL=OFF`.  So the file compiled in
# nobody's build and its assertions were never evaluated.
#
# That matters more than it sounds.  `buildSsaModule` is the single producer of
# the SSA that every structural detector consumes, and the detectors' own unit
# tests hand-build their fixtures -- so a defect in the adapter is invisible to
# the whole suite.  One was: `IrInstr::defValue` was never assigned and only
# ConstantInt operands ever became uses, on four instruction classes, which left
# every def-use predicate dead on production input while the tests stayed green.
#
# What it does
# ------------
# Compiles the adapter, the SSA library and the adapter's tests against whatever
# LLVM `llvm-config` reports, with the repository's own lite gtest, and runs
# them.  That is a different LLVM from the pinned `llvm-project` 23.1.0 the full
# build uses, and deliberately so: the API this adapter touches -- `Instruction`,
# `Value`, `ConstantInt`, `PHINode` -- is the stable part, and a check that runs
# on a plain developer machine is worth more than one that needs a two-hour
# dependency build.  If the adapter ever reaches for something version-specific,
# this stops compiling and says so, which is itself the signal.
#
# It fails, rather than skipping, when no LLVM is found.  A check that quietly
# turns itself off is a check nobody can rely on.
#
# Usage: bash scripts/ci/check_llvm_adapter.sh [--llvm-config PATH] [--keep]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

LLVM_CONFIG=""
KEEP=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--keep) KEEP=1; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 2 ;;
	esac
done

# Newest first, so a machine with several versions uses the most recent.
if [[ -z "${LLVM_CONFIG}" ]]; then
	for c in $(compgen -c llvm-config 2>/dev/null | sort -Vru) llvm-config; do
		if command -v "${c}" >/dev/null 2>&1; then LLVM_CONFIG="${c}"; break; fi
	done
fi

if [[ -z "${LLVM_CONFIG}" ]] || ! command -v "${LLVM_CONFIG}" >/dev/null 2>&1; then
	echo "ADAPT-01: no llvm-config found." >&2
	echo "ADAPT-01: this check builds src/retdec/llvm_to_ssa.cpp, which needs the" >&2
	echo "          LLVM headers and libraries. Install them (Debian/Ubuntu:" >&2
	echo "          'apt-get install llvm-dev', which provides llvm-config) or" >&2
	echo "          pass --llvm-config PATH." >&2
	exit 1
fi

LLVM_VERSION="$("${LLVM_CONFIG}" --version)"
echo "ADAPT-01: building against LLVM ${LLVM_VERSION} (${LLVM_CONFIG})"

WORK="$(mktemp -d)"
if [[ "${KEEP}" -eq 0 ]]; then
	trap 'rm -rf "${WORK}"' EXIT
else
	echo "ADAPT-01: keeping ${WORK}"
fi

# Only the include and define flags: llvm-config also reports the warning and
# standard flags its own build used, and those fight with this one's.
mapfile -t LLVM_CPPFLAGS < <("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' || true)
mapfile -t LLVM_LDFLAGS < <("${LLVM_CONFIG}" --ldflags | tr ' ' '\n' | grep -v '^$' || true)
mapfile -t LLVM_LIBS < <("${LLVM_CONFIG}" --libs core irreader support | tr ' ' '\n' | grep -v '^$' || true)

SRCS=(
	tests/retdec/llvm_to_ssa_test.cpp
	tests/standalone/gtest_lite.cpp
	tests/standalone/gtest_lite_main.cpp
	src/retdec/llvm_to_ssa.cpp
)
mapfile -t SSA_SRCS < <(find src/ssa -name '*.cpp' | sort)
SRCS+=("${SSA_SRCS[@]}")

BIN="${WORK}/llvm_to_ssa_test"
if ! "${CXX:-g++}" -std=c++17 -O0 -g \
	-Iinclude -Isrc/retdec -Itests/standalone \
	"${LLVM_CPPFLAGS[@]}" -fexceptions -frtti \
	"${SRCS[@]}" -o "${BIN}" \
	"${LLVM_LDFLAGS[@]}" "${LLVM_LIBS[@]}" -lpthread -ldl -lz -ltinfo \
	> "${WORK}/build.log" 2>&1
then
	echo "ADAPT-01: FAIL the adapter did not build against LLVM ${LLVM_VERSION}" >&2
	grep -E 'error:' "${WORK}/build.log" | head -n 20 >&2 || tail -n 20 "${WORK}/build.log" >&2
	exit 1
fi

if ! "${BIN}" > "${WORK}/run.log" 2>&1; then
	echo "ADAPT-01: FAIL the adapter's tests do not pass" >&2
	tail -n 40 "${WORK}/run.log" >&2
	exit 1
fi

# The count is printed so a suite that silently stops registering tests is
# visible; "0 tests ran" passes every assertion it has.
RAN="$(grep -aoE '[0-9]+ tests? ran' "${WORK}/run.log" | tail -n1 || true)"
if [[ -z "${RAN}" ]]; then
	echo "ADAPT-01: FAIL could not tell how many tests ran" >&2
	tail -n 20 "${WORK}/run.log" >&2
	exit 1
fi
COUNT="${RAN%% *}"
if [[ "${COUNT}" -lt 1 ]]; then
	echo "ADAPT-01: FAIL no tests ran" >&2
	exit 1
fi

echo "ADAPT-01: OK ${RAN} against LLVM ${LLVM_VERSION}"
