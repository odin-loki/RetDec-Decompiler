#!/usr/bin/env bash
# ORPH-01 -- run the test suites that ctest does not register and no script covers.
#
# Why this exists
# ---------------
# Sixteen directories under tests/ build a gtest executable, install it, and
# never register a single test with ctest: none of their CMakeLists calls
# add_test or gtest_discover_tests. They are exactly the upstream Avast
# directories; every test directory this fork added is registered. Between them
# they hold 6,617 test cases.
#
# Nine of the sixteen are covered by a check in this directory instead --
# B2L-01, L2H-01, C2L-01, DEM-01, FF-01 and the rest -- which is how a suite
# ends up running without ctest knowing about it. SEVEN are not covered by
# anything, and tests/unpacker is an eighth that is deliberately out (it links
# retdec::cpdetect, which needs YARA).
#
# Those seven hold 460 assertions that ran nowhere at all. They pass. This runs
# them.
#
# Each directory is linked into its OWN binary, which is not an incidental
# choice. Linked together, gtest refuses nine of the tests outright --
# "All tests in the same test suite must use the same test fixture class" --
# because two directories define a fixture with the same name. That is an
# artefact of putting them in one binary, not a defect in any of them, and a
# harness that reported it as one would be lying about the code.
#
# The object archive
# ------------------
# This links against the archive B2L-01 builds, rather than compiling the tree
# a second time, so it needs a B2L-01 work directory. Give it one with
# --b2l-workdir; check_push_gates.sh and standalone-check.yml pass the same
# directory to both.
#
# Usage: bash scripts/ci/check_orphan_test_suites.sh --b2l-workdir DIR
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

B2L_DIR=""
while [ $# -gt 0 ]; do
	case "$1" in
		--b2l-workdir) B2L_DIR="$2"; shift 2 ;;
		--self-test) shift ;;   # the two-way directory check always runs
		*) echo "ORPH-01: unknown argument $1" >&2; exit 2 ;;
	esac
done

die() { echo "ORPH-01: FAIL $*" >&2; exit 1; }

[ -n "${B2L_DIR}" ] \
	|| die "pass --b2l-workdir DIR, the work directory check_bin2llvmir_tests.sh used"
[ -f "${B2L_DIR}/libretdec.a" ] \
	|| die "${B2L_DIR}/libretdec.a is not there; run check_bin2llvmir_tests.sh with the same --workdir first"

# directory|the check that runs it, or the reason it is out.
COVERED="
bin2llvmir|scripts/ci/check_bin2llvmir_tests.sh
capstone2llvmir|scripts/ci/check_capstone2llvmir_tests.sh
llvmir-emul|scripts/ci/check_capstone2llvmir_tests.sh
demangler|scripts/ci/check_demangler_tests.sh
llvmir2hll|scripts/ci/check_llvmir2hll_tests.sh
cpdetect|scripts/ci/check_fileformat_tests.sh
loader|scripts/ci/check_fileformat_tests.sh
utils|scripts/standalone_check.sh
unpacker|deliberately out: links retdec::cpdetect, which needs YARA
"

# The seven this check runs. Kept explicit so that a directory appearing or
# disappearing is a failure rather than a silent change of scope.
EXPECTED_ORPHANS="common config ctypes ctypesparser pdbparser pelib serdes"

# What the tree actually says, computed rather than trusted.
actual=""
for f in tests/*/CMakeLists.txt; do
	d="$(basename "$(dirname "$f")")"
	grep -q 'add_executable' "$f" || continue
	grep -qE 'gtest_discover_tests|add_test' "$f" && continue
	covered=""
	while IFS='|' read -r cd _reason; do
		[ -n "${cd}" ] || continue
		[ "${cd}" = "${d}" ] && covered=1
	done <<< "${COVERED}"
	[ -n "${covered}" ] && continue
	actual="${actual} ${d}"
done
actual="$(echo ${actual} | tr ' ' '\n' | sort | tr '\n' ' ' | sed 's/ $//')"
expected="$(echo ${EXPECTED_ORPHANS} | tr ' ' '\n' | sort | tr '\n' ' ' | sed 's/ $//')"

if [ "${actual}" != "${expected}" ]; then
	echo "ORPH-01: the tree says: ${actual}" >&2
	echo "ORPH-01: this check says: ${expected}" >&2
	die "the set of unregistered, uncovered test directories changed. If a directory was registered with ctest or given a check, drop it from EXPECTED_ORPHANS and from this script's header; if a new one appeared, add it here and run it"
fi

LLVM_CONFIG=""
for c in llvm-config-20 llvm-config-21 llvm-config; do
	command -v "$c" >/dev/null 2>&1 && { LLVM_CONFIG="$c"; break; }
done
[ -n "${LLVM_CONFIG}" ] || die "no llvm-config found"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

LLVM_FLAGS="$("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' | tr '\n' ' ')"
INC="-Iinclude -Isrc -Itests -Ideps/rapidjson/include -Ideps/elfio/include \
	-Ideps/tlsh/include -Ideps/tinyxml2/include -Ideps/stb/include \
	-Ideps/authenticode-parser/include -Ideps/eigen -DRAPIDJSON_HAS_STDSTRING=1"

total=0
for d in ${EXPECTED_ORPHANS}; do
	objs=""
	while IFS= read -r f; do
		o="${WORK}/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')"
		g++ -std=c++17 -O0 -w ${INC} ${LLVM_FLAGS} -c "$f" -o "$o" \
			2>"${o}.err" || { sed -n '1,10p' "${o}.err" >&2; die "tests/$d: $f did not compile"; }
		objs="${objs} ${o}"
	done < <(find "tests/$d" -name '*.cpp' | sort)
	[ -n "${objs}" ] || die "tests/$d has no sources"

	g++ -std=c++17 -O0 -w ${objs} "${B2L_DIR}/dwarf_stub.o" -o "${WORK}/$d.bin" \
		-Wl,--start-group "${B2L_DIR}/libretdec.a" "${B2L_DIR}"/dep/*.o -Wl,--end-group \
		-lgtest -lgtest_main -lgmock \
		$("${LLVM_CONFIG}" --libs) $("${LLVM_CONFIG}" --system-libs) \
		-lcapstone -lz -lcrypto -pthread 2>"${WORK}/$d.link" \
		|| { grep -m 10 -E 'undefined reference|error' "${WORK}/$d.link" >&2; \
		     die "tests/$d did not link"; }

	if ! "${WORK}/$d.bin" > "${WORK}/$d.log" 2>&1; then
		echo "--- tests/$d ---" >&2
		grep -aoE '^\[  FAILED  \] [A-Za-z][A-Za-z0-9_]*\.[A-Za-z0-9_]*' \
			"${WORK}/$d.log" | sort -u >&2
		grep -aE 'Failure$' -A 3 "${WORK}/$d.log" | head -n 30 >&2
		die "tests/$d does not pass"
	fi
	n="$(grep -aoE '[0-9]+ tests? from [0-9]+ test suites? ran' "${WORK}/$d.log" \
		| tail -n1 | grep -oE '^[0-9]+' || echo 0)"
	[ "${n}" -gt 0 ] || die "tests/$d ran 0 tests"
	printf '  %-14s %s tests\n' "$d" "$n"
	total=$((total + n))
done

[ "${total}" -ge 400 ] \
	|| die "only ${total} tests ran across the seven; there were 460 when this was written"

echo "ORPH-01: OK ${total} tests across $(echo ${EXPECTED_ORPHANS} | wc -w) suites nothing else runs"
