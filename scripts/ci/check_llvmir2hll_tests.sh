#!/usr/bin/env bash
# L2H-01 — build and run tests/llvmir2hll against the system LLVM.
#
# Why this exists
# ---------------
# `tests/llvmir2hll` is 158 files and, at the time this was written, 1767
# assertions across 137 suites.  Nothing ran any of them.  The CMake target is
# behind `RETDEC_ENABLE_LLVMIR2HLL_TESTS`, which is behind `RETDEC_TESTS`, and
# `.github/workflows/ctest-linux.yml` configures with `RETDEC_TESTS:BOOL=OFF`.
# So the entire test suite for the component that writes the decompiler's C
# output compiled in nobody's build.
#
# That is the component CC-01 measures from the outside: when
# scripts/ci/check_emitted_c_compiles.sh reported `label 'lab_0x112c' used but
# not defined` on generated_shell_sort-gcc-O2, the defect was in
# GotoCFGOptimizer -- a file with a test suite that had never executed.  A
# corpus check tells you the output is broken; this tells you which pass broke
# it.
#
# What it does
# ------------
# Compiles llvmir2hll and its tests against whatever `llvm-config` reports,
# links them with the system gtest/gmock, and runs them.  That is a different
# LLVM from the pinned llvm-project the full build uses, and deliberately so:
# a check that runs on a plain developer machine in a couple of minutes is
# worth more than one that needs a multi-hour dependency build.  See ADAPT-01
# (scripts/ci/check_llvm_adapter.sh) for the same trade made for the SSA
# adapter.
#
# What it does NOT cover, and why
# -------------------------------
# `src/llvmir2hll/llvm/llvmir2bir_converter/` and `tests/llvmir2hll/llvm/` are
# excluded: they use LLVM APIs that moved between the system LLVM and the
# pinned one (`ConstantPointerNull::getPointerType`, the `LoadInst` ctor
# overload set), so they cannot compile here at all.  Those need the full
# build.  The exclusion is named here rather than silently skipped so the hole
# is visible; --list-excluded prints it.
#
# It fails, rather than skipping, when LLVM or gtest is missing.  A check that
# quietly turns itself off is a check nobody can rely on.
#
# Usage: bash scripts/ci/check_llvmir2hll_tests.sh
#          [--llvm-config PATH] [--jobs N] [--min-tests N] [--keep]
#          [--filter GTEST_FILTER] [--list-excluded]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

LLVM_CONFIG=""
JOBS="$(nproc 2>/dev/null || echo 4)"
# The floor is deliberately well under the 1767 that ran when this was written:
# it is here to catch a suite that stops being linked in, not to be a count
# that has to be updated every time somebody adds a test.
MIN_TESTS=1500
KEEP=0
FILTER=""
LIST_EXCLUDED=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--jobs) JOBS="$2"; shift 2 ;;
		--min-tests) MIN_TESTS="$2"; shift 2 ;;
		--filter) FILTER="$2"; shift 2 ;;
		--keep) KEEP=1; shift ;;
		--list-excluded) LIST_EXCLUDED=1; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 2 ;;
	esac
done

# Anything here is compiled by nothing on this path; each entry says why.
excluded_sources() {
	cat <<'EOF'
src/llvmir2hll/llvm/llvmir2bir_converter*	uses LLVM APIs that differ between the system and pinned LLVM
tests/llvmir2hll/llvm/*	tests for the above
src/utils/binary_path.cpp	needs deps/whereami/whereami.c built as C; nothing here calls it
src/utils/version.cpp	needs the RETDEC_GIT_* defines only the CMake build sets
EOF
}

if [[ "${LIST_EXCLUDED}" -eq 1 ]]; then
	echo "L2H-01: not covered by this check:"
	excluded_sources | while IFS=$'\t' read -r path why; do
		printf '  %-46s %s\n' "${path}" "${why}"
	done
	exit 0
fi

# Newest first, so a machine with several versions uses the most recent.
if [[ -z "${LLVM_CONFIG}" ]]; then
	for c in $(compgen -c llvm-config 2>/dev/null | sort -Vru) llvm-config; do
		if command -v "${c}" >/dev/null 2>&1; then LLVM_CONFIG="${c}"; break; fi
	done
fi

if [[ -z "${LLVM_CONFIG}" ]] || ! command -v "${LLVM_CONFIG}" >/dev/null 2>&1; then
	echo "L2H-01: no llvm-config found." >&2
	echo "        Install the LLVM headers (Debian/Ubuntu: 'apt-get install" >&2
	echo "        llvm-dev') or pass --llvm-config PATH." >&2
	exit 1
fi

GTEST_LIB=""
for d in /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib /usr/lib64; do
	if [[ -f "${d}/libgtest.a" ]] && [[ -f "${d}/libgmock.a" ]]; then
		GTEST_LIB="${d}"; break
	fi
done
if [[ -z "${GTEST_LIB}" ]] || [[ ! -d /usr/include/gtest ]]; then
	echo "L2H-01: gtest and gmock are not installed." >&2
	echo "        tests/llvmir2hll uses TEST_F, MOCK_METHOD and gmock matchers," >&2
	echo "        so the tests/standalone gtest shim cannot build it." >&2
	echo "        Debian/Ubuntu: 'apt-get install libgtest-dev libgmock-dev'." >&2
	exit 1
fi

LLVM_VERSION="$("${LLVM_CONFIG}" --version)"
echo "L2H-01: building against LLVM ${LLVM_VERSION} (${LLVM_CONFIG}), gtest in ${GTEST_LIB}"

WORK="${L2H_WORKDIR:-$(mktemp -d)}"
mkdir -p "${WORK}/obj"
if [[ "${KEEP}" -eq 0 ]] && [[ -z "${L2H_WORKDIR:-}" ]]; then
	trap 'rm -rf "${WORK}"' EXIT
else
	echo "L2H-01: keeping ${WORK}"
fi

# Only the include and define flags: llvm-config also reports the warning and
# standard flags its own build used, and those fight with this one's.
LLVM_CPPFLAGS="$("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' | tr '\n' ' ')"

# -w: this builds against a different LLVM than the project pins, so its
# warnings are not the project's to answer.  scripts/standalone_check.sh is
# where warnings are held to a baseline.
# rapidjson and elfio are vendored in deps/, so no fetch step is needed.
# RAPIDJSON_HAS_STDSTRING mirrors deps/rapidjson/CMakeLists.txt; retdec/serdes
# hands std::string straight to rapidjson and does not compile without it.
CXXFLAGS="-std=c++17 -O0 -g0 -Iinclude -Isrc -Itests"
CXXFLAGS+=" -Ideps/rapidjson/include -DRAPIDJSON_HAS_STDSTRING=1"
CXXFLAGS+=" -Ideps/elfio/include ${LLVM_CPPFLAGS} -w"

# Every source that goes in, minus the exclusions above.
mapfile -t SRCS < <(
	{
		find src/llvmir2hll -name '*.cpp' \
			! -path 'src/llvmir2hll/llvm/llvmir2bir_converter*'
		find src/common src/config src/serdes -name '*.cpp'
		find src/utils -name '*.cpp' \
			! -name 'binary_path.cpp' ! -name 'version.cpp'
		find tests/llvmir2hll -name '*.cpp' ! -path 'tests/llvmir2hll/llvm/*'
	} | sort
)

if [[ "${#SRCS[@]}" -lt 400 ]]; then
	echo "L2H-01: FAIL only ${#SRCS[@]} sources found; the tree does not look" >&2
	echo "        like the one this check was written against." >&2
	exit 1
fi

echo "L2H-01: compiling ${#SRCS[@]} translation units with ${JOBS} job(s)"

cat > "${WORK}/compile-one.sh" <<'ONE'
#!/usr/bin/env bash
src="$1"
o="${WORK}/obj/$(echo "${src}" | tr '/' '_').o"
if [[ -f "${o}" ]] && [[ "${o}" -nt "${src}" ]]; then exit 0; fi
if ! ${CXX:-g++} ${CXXFLAGS} -c "${src}" -o "${o}" 2> "${o}.log"; then
	echo "FAIL ${src}"
	exit 1
fi
ONE
chmod +x "${WORK}/compile-one.sh"
export WORK CXXFLAGS

if ! printf '%s\n' "${SRCS[@]}" \
	| xargs -P "${JOBS}" -n 1 "${WORK}/compile-one.sh" > "${WORK}/compile.out" 2>&1
then
	echo "L2H-01: FAIL llvmir2hll does not build against LLVM ${LLVM_VERSION}" >&2
	cat "${WORK}/compile.out" >&2
	for log in "${WORK}"/obj/*.o.log; do
		if grep -q 'error:' "${log}" 2>/dev/null; then
			grep -m 5 'error:' "${log}" >&2
		fi
	done
	exit 1
fi

# TargetHLL and friends live in llvmir2hll.cpp alongside the LLVM pass driver,
# which pulls in the converter this check cannot compile.  target_hll.cpp holds
# the one definition the emitter needs, so llvmir2hll.cpp itself stays out.
rm -f "${WORK}/obj/src_llvmir2hll_llvmir2hll.cpp.o"

BIN="${WORK}/llvmir2hll_test"
if ! ${CXX:-g++} -o "${BIN}" "${WORK}"/obj/*.o \
	-L"${GTEST_LIB}" -lgmock -lgtest -lgtest_main \
	$("${LLVM_CONFIG}" --ldflags) \
	$("${LLVM_CONFIG}" --libs core support irreader) \
	-lpthread -ldl -lz -ltinfo > "${WORK}/link.log" 2>&1
then
	echo "L2H-01: FAIL the tests do not link" >&2
	grep -E 'undefined reference|error' "${WORK}/link.log" | head -n 20 >&2
	exit 1
fi

RUN_ARGS=()
if [[ -n "${FILTER}" ]]; then
	RUN_ARGS+=("--gtest_filter=${FILTER}")
fi

if ! "${BIN}" "${RUN_ARGS[@]}" > "${WORK}/run.log" 2>&1; then
	echo "L2H-01: FAIL tests/llvmir2hll does not pass" >&2
	grep -E '^\[  FAILED  \]|Failure$' "${WORK}/run.log" | head -n 40 >&2
	echo "--- context ---" >&2
	tail -n 40 "${WORK}/run.log" >&2
	exit 1
fi

# "0 tests ran" passes every assertion it has, so the count is checked, not
# just the exit status.
RAN="$(grep -aoE '[0-9]+ tests? from [0-9]+ test suites? ran' "${WORK}/run.log" | tail -n1 || true)"
if [[ -z "${RAN}" ]]; then
	echo "L2H-01: FAIL could not tell how many tests ran" >&2
	tail -n 20 "${WORK}/run.log" >&2
	exit 1
fi
COUNT="${RAN%% *}"
if [[ -z "${FILTER}" ]] && [[ "${COUNT}" -lt "${MIN_TESTS}" ]]; then
	echo "L2H-01: FAIL only ${COUNT} tests ran, expected at least ${MIN_TESTS}" >&2
	echo "        A suite stopped being linked in, or a test file was dropped." >&2
	exit 1
fi

echo "L2H-01: OK ${RAN} against LLVM ${LLVM_VERSION}"
