#!/usr/bin/env bash
# FF-01 — build and run tests/cpdetect, tests/loader and tests/fileformat against
# the system LLVM.
#
# Why this exists
# ---------------
# These suites -- the layer that turns a file on disk into sections, segments
# and symbols -- ran in no gate at all, for the same two reasons as L2H-01,
# SEM-01 and DEM-01: standalone_check.sh skips anything that needs a
# third-party library, and ctest-linux neither builds the test targets nor
# selects them (gtest_discover_tests attaches no labels, so `ctest -L unit`
# reaches only the three tests/ directories that set LABELS themselves).
#
# tests/fileformat was the half of that hole this check left open. It is named
# in standalone_check.sh's SUITES, which reads as covered, but its
# PARTIAL_SUITES entry builds four of its sixteen files -- the lattice, the DER
# decoder, CharacterIterator and the elfio bounds test, the ones that need no
# LLVM. The other twelve drive retdec::fileformat itself: the ELF, PE, COFF,
# Mach-O, Intel HEX, raw-data and AR readers, format detection and the format
# factory. Counted before this check picked them up: 46 of the suite's 129 test
# cases ran somewhere and 83 ran nowhere.
#
# They cost nothing to add. src/fileformat is already compiled here -- cpdetect
# and loader link it -- and all sixteen files compile against the distribution
# llvm-dev, measured with -fsyntax-only against LLVM 18.1.3 before the floor
# below was set.
#
# src/fileformat was assumed to need the pinned llvm-project. It does not: it
# uses llvm/Object/COFF.h, llvm/Object/Archive.h and a handful of
# llvm/Support headers, all of which the distribution llvm-dev provides. What
# it also needs -- stb, tlsh and authenticode-parser -- turned out to be
# vendored in deps/ with sources, not download stubs, so nothing has to be
# fetched.
#
# tests/unpacker is deliberately NOT here: it links retdec::cpdetect, which
# needs YARA, which is a real download. It stays on the UNGATED_SUITES list.
#
# It fails, rather than skipping, when LLVM or gmock is missing. A check that
# quietly turns itself off is a check nobody can rely on.
#
# Usage: bash scripts/ci/check_fileformat_tests.sh [--llvm-config PATH]
#                                                  [--jobs N] [--keep]
#        [--min-cpdetect N] [--min-loader N] [--min-fileformat N]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

LLVM_CONFIG=""
JOBS="$(nproc 2>/dev/null || echo 4)"
MIN_CPDETECT=9
MIN_LOADER=60
MIN_FILEFORMAT=135
KEEP=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--jobs) JOBS="$2"; shift 2 ;;
		--min-cpdetect) MIN_CPDETECT="$2"; shift 2 ;;
		--min-loader) MIN_LOADER="$2"; shift 2 ;;
		--min-fileformat) MIN_FILEFORMAT="$2"; shift 2 ;;
		--keep) KEEP=1; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 2 ;;
	esac
done

if [[ -z "${LLVM_CONFIG}" ]]; then
	for c in $(compgen -c llvm-config 2>/dev/null | sort -Vru) llvm-config; do
		if command -v "${c}" >/dev/null 2>&1; then LLVM_CONFIG="${c}"; break; fi
	done
fi
if [[ -z "${LLVM_CONFIG}" ]] || ! command -v "${LLVM_CONFIG}" >/dev/null 2>&1; then
	echo "FF-01: no llvm-config found." >&2
	echo "FF-01: this check builds src/fileformat, which needs the LLVM headers" >&2
	echo "       and libraries (Debian/Ubuntu: 'apt-get install llvm-dev')." >&2
	exit 1
fi

LLVM_VERSION="$("${LLVM_CONFIG}" --version)"
CXX_BIN="${CXX:-g++}"
CC_BIN="${CC:-gcc}"

WORK="$(mktemp -d)"
if [[ "${KEEP}" -eq 0 ]]; then
	trap 'rm -rf "${WORK}"' EXIT
else
	echo "FF-01: keeping ${WORK}"
fi
mkdir -p "${WORK}/obj"

printf '#include <gmock/gmock.h>\nint main(){ return 0; }\n' > "${WORK}/probe.cpp"
if ! "${CXX_BIN}" -std=c++17 "${WORK}/probe.cpp" -lgmock -lgtest -lpthread \
	-o "${WORK}/probe" > "${WORK}/probe.log" 2>&1
then
	echo "FF-01: no usable gmock found." >&2
	echo "FF-01: (Debian/Ubuntu: 'apt-get install libgtest-dev libgmock-dev')." >&2
	tail -n 10 "${WORK}/probe.log" >&2
	exit 1
fi

echo "FF-01: building against LLVM ${LLVM_VERSION} (${LLVM_CONFIG})"

mapfile -t LLVM_CPPFLAGS < <("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' || true)
mapfile -t LLVM_LDFLAGS < <("${LLVM_CONFIG}" --ldflags | tr ' ' '\n' | grep -v '^$' || true)
mapfile -t LLVM_LIBS < <("${LLVM_CONFIG}" --libs object support binaryformat demangle | tr ' ' '\n' | grep -v '^$' || true)

INCLUDES=(
	-Iinclude -Itests
	-Ideps/rapidjson/include -Ideps/elfio/include
	-Ideps/stb/include -Ideps/tlsh/include -Ideps/authenticode-parser/include
	-DRAPIDJSON_HAS_STDSTRING=1
)

# authenticode-parser and stb are C. Feeding them to the C++ driver alongside
# the .cpp sources makes g++ compile them as C++, where a `goto` past an
# initialisation is an error rather than a warning.
C_SRCS=()
while IFS= read -r f; do C_SRCS+=("$f"); done < <(find deps/authenticode-parser/src -name '*.c' | sort)
C_SRCS+=(deps/stb/stb_image.c)
C_OBJS=()
for f in "${C_SRCS[@]}"; do
	o="${WORK}/obj/$(printf '%s' "$f" | tr '/' '_').o"
	if ! "${CC_BIN}" -std=c11 -O0 -g0 -w \
		-Ideps/authenticode-parser/include -Ideps/stb/include \
		-c "$f" -o "$o" > "${WORK}/cbuild.log" 2>&1
	then
		echo "FF-01: FAIL a vendored C dependency did not build: $f" >&2
		tail -n 20 "${WORK}/cbuild.log" >&2
		exit 1
	fi
	C_OBJS+=("$o")
done

# binary_path.cpp needs deps/whereami, version.cpp needs the version macros the
# top-level CMakeLists stamps in, gpu_scanner_cpu.cpp needs the CUDA stubs.
# Matched on the whole basename: `grep -v version.cpp` also drops
# CONversion.cpp, and the link then fails three files away from the mistake.
CXX_SRCS=()
while IFS= read -r f; do CXX_SRCS+=("$f"); done < <(
	find src/fileformat src/loader src/common src/pelib src/serdes -name '*.cpp' | sort
)
while IFS= read -r f; do CXX_SRCS+=("$f"); done < <(
	find src/utils -name '*.cpp' \
		! -name 'binary_path.cpp' ! -name 'version.cpp' \
		! -name 'gpu_scanner_cpu.cpp' | sort
)
while IFS= read -r f; do CXX_SRCS+=("$f"); done < <(
	find deps/tlsh -name '*.cpp' ! -name 'WinFunctions.cpp' | sort
)

build_and_run() {
	local suite="$1" floor="$2"
	local srcs=("${CXX_SRCS[@]}")
	while IFS= read -r f; do srcs+=("$f"); done < <(find "tests/${suite}" -name '*.cpp' | sort)

	local bin="${WORK}/${suite}_test"
	if ! "${CXX_BIN}" -std=c++17 -O0 -g0 "${INCLUDES[@]}" "${LLVM_CPPFLAGS[@]}" -w \
		"${srcs[@]}" "${C_OBJS[@]}" -o "${bin}" \
		"${LLVM_LDFLAGS[@]}" "${LLVM_LIBS[@]}" \
		-lgmock_main -lgmock -lgtest -lpthread -lcrypto -lz \
		> "${WORK}/${suite}.build.log" 2>&1
	then
		echo "FF-01: FAIL tests/${suite} did not build against LLVM ${LLVM_VERSION}" >&2
		grep -E 'error:|undefined reference' "${WORK}/${suite}.build.log" | head -n 20 >&2 \
			|| tail -n 20 "${WORK}/${suite}.build.log" >&2
		return 1
	fi

	if ! "${bin}" > "${WORK}/${suite}.run.log" 2>&1; then
		echo "FF-01: FAIL the ${suite} tests do not pass" >&2
		grep -E '^\[  FAILED  \]' "${WORK}/${suite}.run.log" | head -n 20 >&2 \
			|| tail -n 40 "${WORK}/${suite}.run.log" >&2
		return 1
	fi

	local ran count
	ran="$(grep -aoE '[0-9]+ tests? from [0-9]+ test suites? ran' "${WORK}/${suite}.run.log" | tail -n1 || true)"
	if [[ -z "${ran}" ]]; then
		echo "FF-01: FAIL could not tell how many ${suite} tests ran" >&2
		tail -n 20 "${WORK}/${suite}.run.log" >&2
		return 1
	fi
	count="${ran%% *}"
	if [[ "${count}" -lt "${floor}" ]]; then
		echo "FF-01: FAIL only ${count} ${suite} tests ran, expected at least ${floor}" >&2
		echo "       If the suite shrank on purpose, lower the floor and say why." >&2
		return 1
	fi
	echo "FF-01: ${suite}: ${ran}"
}

status=0
build_and_run cpdetect "${MIN_CPDETECT}" || status=1
build_and_run loader "${MIN_LOADER}" || status=1
build_and_run fileformat "${MIN_FILEFORMAT}" || status=1
[[ "${status}" -eq 0 ]] || exit 1

echo "FF-01: OK against LLVM ${LLVM_VERSION}"
