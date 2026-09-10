#!/usr/bin/env bash
# DEM-01 — build and run tests/demangler against the system LLVM.
#
# Why this exists
# ---------------
# tests/demangler is 125 assertions over the component that turns mangled
# symbols back into declarations, and they ran in no gate at all.
#
#   * scripts/standalone_check.sh builds only the modules that need no
#     third-party library, and src/demangler includes <llvm/Demangle/...>.
#   * ctest-linux configures with RETDEC_TESTS=ON, so `tests-demangler` is a
#     configured target -- but the build step names its targets explicitly and
#     this is not one of them, and gtest_discover_tests attaches NO labels, so
#     the `ctest -L unit` step selects it either way. Measured with a two-test
#     probe project: an unbuilt discovered target shows as
#     `<target>_NOT_BUILT` under plain `ctest -N` and is absent under
#     `ctest -L unit -N`; a BUILT one is still absent under `-L unit`.
#
# The demangler is not incidental to this tool: every C++ symbol in a binary
# reaches the output through it, and src/compiler_abi carries a second,
# hand-written Itanium parser whose length accumulator overflowed an int for
# ten years' worth of digits. That one was found because compiler_abi IS in a
# gate. This one was not in any.
#
# What it does
# ------------
# Compiles src/demangler, the ctypes layer it parses into, and the suite
# against whatever LLVM `llvm-config` reports, and runs them. That is a
# different LLVM from the pinned llvm-project the full build uses, and
# deliberately so: llvm/Demangle is among the most stable corners of LLVM, and
# a check that runs on a plain developer machine is worth more than one that
# needs a two-hour dependency build. If the demangler ever reaches for
# something version-specific, this stops compiling and says so.
#
# gtest and gmock come from the system because the suite uses gmock matchers,
# which the tests/standalone shim does not implement -- the same reason L2H-01
# takes them from apt.
#
# It fails, rather than skipping, when LLVM or gmock is missing. A check that
# quietly turns itself off is a check nobody can rely on.
#
# Usage: bash scripts/ci/check_demangler_tests.sh [--llvm-config PATH]
#                                                 [--min-tests N] [--keep]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

LLVM_CONFIG=""
MIN_TESTS=120
KEEP=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--min-tests) MIN_TESTS="$2"; shift 2 ;;
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
	echo "DEM-01: no llvm-config found." >&2
	echo "DEM-01: this check builds src/demangler, which needs the LLVM headers" >&2
	echo "        and libraries (Debian/Ubuntu: 'apt-get install llvm-dev')." >&2
	exit 1
fi

LLVM_VERSION="$("${LLVM_CONFIG}" --version)"
CXX_BIN="${CXX:-g++}"

WORK="$(mktemp -d)"
if [[ "${KEEP}" -eq 0 ]]; then
	trap 'rm -rf "${WORK}"' EXIT
else
	echo "DEM-01: keeping ${WORK}"
fi

# Probe for gmock before building the real thing, so "no gmock" reports itself
# as that rather than as a wall of missing-header errors.
printf '#include <gmock/gmock.h>\nint main(){ return 0; }\n' > "${WORK}/probe.cpp"
if ! "${CXX_BIN}" -std=c++17 "${WORK}/probe.cpp" -lgmock -lgtest -lpthread \
	-o "${WORK}/probe" > "${WORK}/probe.log" 2>&1
then
	echo "DEM-01: no usable gmock found." >&2
	echo "DEM-01: the suite uses gmock matchers, which the tests/standalone shim" >&2
	echo "        does not implement (Debian/Ubuntu: 'apt-get install" >&2
	echo "        libgtest-dev libgmock-dev')." >&2
	tail -n 10 "${WORK}/probe.log" >&2
	exit 1
fi

echo "DEM-01: building against LLVM ${LLVM_VERSION} (${LLVM_CONFIG})"

# Only the include and define flags: llvm-config also reports the warning and
# standard flags its own build used, and those fight with this one's.
mapfile -t LLVM_CPPFLAGS < <("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' || true)
mapfile -t LLVM_LDFLAGS < <("${LLVM_CONFIG}" --ldflags | tr ' ' '\n' | grep -v '^$' || true)
mapfile -t LLVM_LIBS < <("${LLVM_CONFIG}" --libs demangle support | tr ' ' '\n' | grep -v '^$' || true)

SRCS=()
while IFS= read -r f; do SRCS+=("$f"); done < <(
	find src/demangler src/ctypesparser src/ctypes -name '*.cpp' | sort
)

# binary_path.cpp needs deps/whereami, version.cpp needs the version macros the
# top-level CMakeLists stamps in, and the gpu_scanner pair needs the CUDA
# stubs. None of the three is reachable from the demangler. Matched on the
# whole basename: a `grep -v version.cpp` also drops CONversion.cpp, which is
# where double10ToDouble8 lives, and the link then fails somewhere else
# entirely.
while IFS= read -r f; do SRCS+=("$f"); done < <(
	find src/utils -name '*.cpp' \
		! -name 'binary_path.cpp' \
		! -name 'version.cpp' \
		! -name 'gpu_scanner_cpu.cpp' | sort
)
while IFS= read -r f; do SRCS+=("$f"); done < <(find tests/demangler -name '*.cpp' | sort)

BIN="${WORK}/demangler_test"
if ! "${CXX_BIN}" -std=c++17 -O0 -g0 \
	-Iinclude -Itests -Ideps/rapidjson/include -DRAPIDJSON_HAS_STDSTRING=1 \
	"${LLVM_CPPFLAGS[@]}" -w \
	"${SRCS[@]}" -o "${BIN}" \
	"${LLVM_LDFLAGS[@]}" "${LLVM_LIBS[@]}" \
	-lgmock_main -lgmock -lgtest -lpthread \
	> "${WORK}/build.log" 2>&1
then
	echo "DEM-01: FAIL the demangler did not build against LLVM ${LLVM_VERSION}" >&2
	grep -E 'error:|undefined reference' "${WORK}/build.log" | head -n 20 >&2 \
		|| tail -n 20 "${WORK}/build.log" >&2
	exit 1
fi

if ! "${BIN}" > "${WORK}/run.log" 2>&1; then
	echo "DEM-01: FAIL the demangler tests do not pass" >&2
	grep -E '^\[  FAILED  \]|FAILED' "${WORK}/run.log" | head -n 20 >&2 \
		|| tail -n 40 "${WORK}/run.log" >&2
	exit 1
fi

# The count is printed so a suite that silently stops registering tests is
# visible; "0 tests ran" passes every assertion it has. --min-tests is the
# floor: it is what stops a suite from shrinking without anyone noticing.
RAN="$(grep -aoE '[0-9]+ tests? from [0-9]+ test suites? ran' "${WORK}/run.log" | tail -n1 || true)"
if [[ -z "${RAN}" ]]; then
	echo "DEM-01: FAIL could not tell how many tests ran" >&2
	tail -n 20 "${WORK}/run.log" >&2
	exit 1
fi
COUNT="${RAN%% *}"
if [[ "${COUNT}" -lt "${MIN_TESTS}" ]]; then
	echo "DEM-01: FAIL only ${COUNT} tests ran, expected at least ${MIN_TESTS}" >&2
	echo "        If the suite shrank on purpose, lower --min-tests and say why." >&2
	exit 1
fi

echo "DEM-01: OK ${RAN} against LLVM ${LLVM_VERSION}"
