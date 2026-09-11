#!/usr/bin/env bash
# C2L-01 — build src/capstone2llvmir and run tests/capstone2llvmir, per
# architecture.
#
# Why this exists
# ---------------
# tests/capstone2llvmir is 46,066 lines and 4,370 assertions -- the only place
# in this repository where ARM, ARM64, MIPS and PowerPC instruction semantics
# are checked at all -- and it was on standalone_check.sh's UNGATED_SUITES
# debt list, evaluated nowhere. The recorded reason was two things:
#
#   "same LLVM 20+ drift (4-arg APInt, Intrinsic::getOrInsertDeclaration), and
#    its tests need <keystone/keystone.h>, which deps/keystone is a download
#    stub for"
#
# Both are facts about the machine, not about the code. llvm-20-dev is in
# Ubuntu noble's own universe pocket, and Keystone 0.9.2 and Capstone 5.0.9 --
# the exact revisions cmake/deps.cmake pins -- build from source in about three
# minutes between them. With those present, all 17 translator sources and all
# six test files compile with no errors and all 4,370 tests pass. The suite was
# never broken; nothing had ever run it.
#
# The system Capstone is not enough: it is 4.0.2, and the x86 translator names
# X86_REG_BND0..3, X86_INS_FUCOMPI and X86_INS_FCOMPI, which arrived in 5.x.
# That is six errors, and taking them as "the code has drifted" rather than
# "this is the wrong Capstone" is how the entry above came to be written.
#
# Why the floors are per architecture
# -----------------------------------
# The parity question this answers is not "do the tests pass" but "is every
# architecture still being asked". A single total lets 1,965 x86 cases hide the
# disappearance of all 598 MIPS ones, and the aggregate would barely move. Each
# architecture therefore has its own floor, and each is checked separately.
#
# Usage: bash scripts/ci/check_capstone2llvmir_tests.sh
#            [--llvm-config PATH] [--jobs N] [--keep] [--self-test]
#            [--deps-dir DIR]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JOBS="$(nproc 2>/dev/null || echo 4)"
KEEP=0
SELF_TEST=0
LLVM_CONFIG=""
DEPS_DIR="${C2L_DEPS_DIR:-/tmp/c2l-deps}"
WORKDIR="${C2L_WORKDIR:-}"

# The measured counts, per architecture. A floor, not an equality: adding
# tests should not fail the gate, losing them must.
readonly MIN_X86=1965
readonly MIN_ARM=534
readonly MIN_ARM64=466
readonly MIN_MIPS=598
readonly MIN_POWERPC=808
readonly MIN_EMUL=10

# The revisions cmake/deps.cmake pins. Kept in step with it by CI: if they
# diverge, this gate is testing something the product does not use.
readonly CAPSTONE_TAG=5.0.9
readonly KEYSTONE_TAG=0.9.2

while [ $# -gt 0 ]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--jobs)        JOBS="$2"; shift 2 ;;
		--deps-dir)    DEPS_DIR="$2"; shift 2 ;;
		--keep)        KEEP=1; shift ;;
		--self-test)   SELF_TEST=1; shift ;;
		*) echo "C2L-01: unknown option: $1" >&2; exit 2 ;;
	esac
done

die() { echo "C2L-01: FAIL $*" >&2; exit 1; }

cd "${ROOT}"

# ── The pinned revisions must match cmake/deps.cmake ─────────────────────────
# A gate that builds a different Capstone from the one the product ships is
# measuring a different decompiler.
cs_pinned="$(sed -n 's|.*capstone/archive/refs/tags/\([0-9.]*\)\.zip.*|\1|p' cmake/deps.cmake | head -1)"
ks_pinned="$(sed -n 's|.*keystone/archive/refs/tags/\([0-9.]*\)\.zip.*|\1|p' cmake/deps.cmake | head -1)"
[ "${cs_pinned}" = "${CAPSTONE_TAG}" ] \
	|| die "cmake/deps.cmake pins Capstone ${cs_pinned}, this gate builds ${CAPSTONE_TAG}"
[ "${ks_pinned}" = "${KEYSTONE_TAG}" ] \
	|| die "cmake/deps.cmake pins Keystone ${ks_pinned}, this gate builds ${KEYSTONE_TAG}"

# ── No duplicate keys in any _i2fm ───────────────────────────────────────────
# Each translator's instruction map is a std::map built from an initializer
# list, and an initializer list keeps the FIRST entry for a repeated key and
# discards the rest -- silently, with no warning from any compiler. So a second
# entry added for an instruction that already has one does nothing at all, and
# looks exactly like a fix. That happened while writing this: an ARM64_INS_HINT
# entry added near the NOPs was dead on arrival behind a nullptr entry 300
# lines earlier.
for init in src/capstone2llvmir/*/*_init.cpp; do
	dups="$(grep -oE '\{[A-Z0-9]+_INS_[A-Z0-9_]+,' "${init}" | sort | uniq -d || true)"
	if [ -n "${dups}" ]; then
		echo "C2L-01: FAIL ${init} maps an instruction more than once; the later" >&2
		echo "        entry is discarded by the std::map initializer list:" >&2
		printf '          %s\n' ${dups} >&2
		exit 1
	fi
done

# ── LLVM ─────────────────────────────────────────────────────────────────────
if [ -z "${LLVM_CONFIG}" ]; then
	for c in llvm-config-20 llvm-config-21 llvm-config; do
		command -v "$c" >/dev/null 2>&1 && { LLVM_CONFIG="$c"; break; }
	done
fi
[ -n "${LLVM_CONFIG}" ] || die "no llvm-config found -- install llvm-20-dev"
LLVM_MAJOR="$("${LLVM_CONFIG}" --version | cut -d. -f1)"
[ "${LLVM_MAJOR}" -ge 20 ] \
	|| die "${LLVM_CONFIG} is LLVM ${LLVM_MAJOR}; this needs 20 or newer (install llvm-20-dev)"

# ── Capstone and Keystone, at the pinned revisions ───────────────────────────
mkdir -p "${DEPS_DIR}"
build_dep() {
	local name="$1" tag="$2" url="$3" prefix="$4"; shift 4
	if [ -f "${prefix}/lib/lib${name}.a" ]; then
		echo "C2L-01: ${name} ${tag} already built in ${prefix}"
		return 0
	fi
	local src="${DEPS_DIR}/${name}-${tag}"
	if [ ! -d "${src}/.git" ]; then
		rm -rf "${src}"
		echo "C2L-01: cloning ${name} ${tag}"
		GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch "${tag}" "${url}" "${src}" \
			>/dev/null 2>&1 || die "could not clone ${name} ${tag} from ${url}"
	fi
	echo "C2L-01: building ${name} ${tag}"
	cmake -S "${src}" -B "${src}/build" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		"$@" >/dev/null 2>&1 || die "cmake configure failed for ${name}"
	cmake --build "${src}/build" -j "${JOBS}" >/dev/null 2>&1 \
		|| die "build failed for ${name}"
	cmake --install "${src}/build" >/dev/null 2>&1 || die "install failed for ${name}"
	[ -f "${prefix}/lib/lib${name}.a" ] || die "${name} produced no static library"
}

CS_PREFIX="${DEPS_DIR}/capstone-install"
KS_PREFIX="${DEPS_DIR}/keystone-install"
build_dep capstone "${CAPSTONE_TAG}" \
	"https://github.com/capstone-engine/capstone" "${CS_PREFIX}" \
	-DCAPSTONE_BUILD_SHARED_LIBS=OFF -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF
build_dep keystone "${KEYSTONE_TAG}" \
	"https://github.com/keystone-engine/keystone" "${KS_PREFIX}" \
	-DBUILD_LIBS_ONLY=1

# ── Every vector arrangement capstone defines is handled ─────────────────────
# extractVectorValue() switches on the ARM64 vector arrangement specifier and
# throws on anything it does not name -- and nothing catches that throw, so a
# single unhandled arrangement ends the decompilation of the whole binary
# rather than of one instruction. ARM64_VAS_2D was the one of capstone's
# fifteen that was missing, and it is why every ARM64 binary in the ARCH-01
# corpus produced no output at all: glibc's string and math routines are full
# of `.2d` operands.
#
# A C enum switched on with a `default` gets no -Wswitch help, so the
# exhaustiveness is checked here instead of by the compiler.
vas_hdr="${CS_PREFIX}/include/capstone/arm64.h"
if [ -f "${vas_hdr}" ]; then
	vas_missing=""
	for v in $(grep -oE 'ARM64_VAS_[A-Z0-9_]+' "${vas_hdr}" | sort -u); do
		grep -q "${v}" src/capstone2llvmir/arm64/arm64.cpp || vas_missing="${vas_missing} ${v}"
	done
	if [ -n "${vas_missing}" ]; then
		echo "C2L-01: FAIL arm64.cpp does not name every ARM64_VAS_* capstone" >&2
		echo "        defines; each unnamed one throws out of extractVectorValue" >&2
		echo "        and ends the whole decompilation:${vas_missing}" >&2
		exit 1
	fi
fi

# ── Compile ──────────────────────────────────────────────────────────────────
if [ -z "${WORKDIR}" ]; then
	WORKDIR="$(mktemp -d -t c2l-check-XXXXXX)"
	[ "${KEEP}" = 1 ] || trap 'rm -rf "${WORKDIR}"' EXIT
else
	mkdir -p "${WORKDIR}"
fi
mkdir -p "${WORKDIR}/obj"

LLVM_FLAGS="$("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' | tr '\n' ' ')"
INC="-Iinclude -Isrc -Itests -I${CS_PREFIX}/include -I${KS_PREFIX}/include"
INC="${INC} -Ideps/rapidjson/include -DRAPIDJSON_HAS_STDSTRING=1"

srcs="${WORKDIR}/srcs.txt"
{
	find src/capstone2llvmir src/common src/config src/serdes src/llvmir-emul \
		-name '*.cpp'
	# src/utils, minus the two that need generated version headers and the CUDA
	# scanner, none of which this binary calls.
	find src/utils -name '*.cpp' \
		! -name 'binary_path.cpp' ! -name 'version.cpp' ! -name 'gpu_scanner*'
	find src/profiling -name '*.cpp'
	ls tests/capstone2llvmir/*.cpp
	# tests/llvmir-emul rides along. It was on the same debt list for the same
	# reason ("uses the 4-argument APInt ctor (LLVM 20+) ... ten tests is a
	# thin return for that risk"), src/llvmir-emul is already linked into this
	# binary because the translator tests execute the IR they produce, and the
	# suite compiles here unchanged. Ten assertions for one line.
	ls tests/llvmir-emul/*.cpp
} | sort -u > "${srcs}"

n_srcs="$(wc -l < "${srcs}")"
[ "${n_srcs}" -ge 60 ] || die "expected at least 60 sources, found ${n_srcs}"
echo "C2L-01: compiling ${n_srcs} translation units against LLVM ${LLVM_MAJOR} with ${JOBS} job(s)"

: > "${WORKDIR}/cc.err"
export WORKDIR
xargs -P "${JOBS}" -I{} sh -c '
	o="${WORKDIR}/obj/$(echo "$1" | tr "/" "_").o"
	g++ -std=c++17 -c -O0 '"${INC} ${LLVM_FLAGS}"' "$1" -o "$o" \
		2>>"${WORKDIR}/cc.err" || echo "FAILED $1" >> "${WORKDIR}/cc.err"
' _ {} < "${srcs}"

if grep -q '^FAILED ' "${WORKDIR}/cc.err"; then
	echo "C2L-01: FAIL these do not compile:" >&2
	grep '^FAILED ' "${WORKDIR}/cc.err" >&2
	grep 'error:' "${WORKDIR}/cc.err" | head -20 >&2
	exit 1
fi

# ── Link and run ─────────────────────────────────────────────────────────────
g++ -o "${WORKDIR}/c2l_tests" "${WORKDIR}"/obj/*.o \
	-L"${CS_PREFIX}/lib" -lcapstone -L"${KS_PREFIX}/lib" -lkeystone \
	$("${LLVM_CONFIG}" --libs) $("${LLVM_CONFIG}" --system-libs) \
	-lgtest -lgmock -lgtest_main -pthread 2>"${WORKDIR}/link.err" \
	|| { grep -oE "undefined reference to .[^']*'" "${WORKDIR}/link.err" \
		| sort -u | head -20 >&2; die "link failed"; }

set +e
"${WORKDIR}/c2l_tests" --gtest_brief=1 > "${WORKDIR}/run.log" 2>&1
run_status=$?
set -e

if [ "${run_status}" != 0 ]; then
	grep -E '^\[  FAILED  \]' "${WORKDIR}/run.log" | head -20 >&2
	die "tests/capstone2llvmir does not pass"
fi

# ── Per-architecture floors ──────────────────────────────────────────────────
count_arch() {
	# `grep -c` exits 1 when it counts nothing, and under `set -e` that killed
	# this script at the assignment -- so an architecture whose suite had gone
	# missing entirely, the one case these floors exist for, exited without
	# printing which one it was. `|| true` keeps the 0 and the message.
	"${WORKDIR}/c2l_tests" --gtest_list_tests \
		--gtest_filter="*Capstone2LlvmIrTranslator$1Tests.*" 2>/dev/null \
		| grep -cE '^  [A-Za-z]' || true
}

bad=0
for pair in "X86:${MIN_X86}" "Arm:${MIN_ARM}" "Arm64:${MIN_ARM64}" \
            "Mips:${MIN_MIPS}" "Powerpc:${MIN_POWERPC}"; do
	arch="${pair%%:*}"; floor="${pair##*:}"
	got="$(count_arch "${arch}")"
	if [ "${got}" -lt "${floor}" ]; then
		echo "C2L-01: FAIL ${arch}: ${got} test(s), floor is ${floor}" >&2
		bad=1
	else
		printf 'C2L-01:   %-8s %5d test(s)\n' "${arch}" "${got}"
	fi
done
emul="$("${WORKDIR}/c2l_tests" --gtest_list_tests --gtest_filter='LlvmIrEmul*.*' \
	2>/dev/null | grep -cE '^  [A-Za-z]' || true)"
if [ "${emul}" -lt "${MIN_EMUL}" ]; then
	echo "C2L-01: FAIL LlvmIrEmul: ${emul} test(s), floor is ${MIN_EMUL}" >&2
	bad=1
else
	printf 'C2L-01:   %-8s %5d test(s)\n' "emul" "${emul}"
fi

[ "${bad}" = 0 ] || die "an architecture lost coverage"

total="$(sed -n 's/.*\[==========\] \([0-9]*\) tests\? from .* ran.*/\1/p' "${WORKDIR}/run.log" | tail -1)"
echo "C2L-01: OK ${total:-0} tests across 5 architectures, against LLVM ${LLVM_MAJOR}, Capstone ${CAPSTONE_TAG}, Keystone ${KEYSTONE_TAG}"

if [ "${SELF_TEST}" = 1 ]; then
	# The failure this gate exists to catch is an architecture's suite going
	# missing -- dropped from the source list, renamed, or not linked -- which
	# makes count_arch return 0 while every remaining test still passes. So
	# exercise exactly that path rather than the arithmetic around it: ask for
	# an architecture that is not there and require both that counting yields
	# 0 and that 0 is rejected by the same comparison the real floors use.
	echo "C2L-01: self-test -- a missing architecture must count 0 and be rejected"
	ghost="$(count_arch NoSuchArchitecture)"
	[ "${ghost}" = 0 ] \
		|| die "self-test: counting an absent architecture gave ${ghost}, not 0"
	if [ "${ghost}" -lt "${MIN_MIPS}" ]; then
		echo "C2L-01: self-test OK (an absent suite counts 0 and fails its floor)"
	else
		die "self-test: 0 was not rejected by a floor of ${MIN_MIPS}"
	fi

	# And the counter must not answer 0 for a suite that IS there, or the check
	# above would pass with every architecture silently gone.
	real="$(count_arch Mips)"
	[ "${real}" -gt 0 ] \
		|| die "self-test: counting a present architecture (Mips) gave 0"
fi
