#!/usr/bin/env bash
# PSEUDO-01 — build the pseudo-assembly probe and measure, per architecture,
# how many of a corpus's instructions come out of src/capstone2llvmir as a
# pseudo-assembly call rather than as semantics.
#
# Why this exists
# ---------------
# COV-01 asks whether the dispatch table has a function pointer for an
# instruction id, which is an upper bound on coverage and not the same
# question. Over the 210-binary parity corpus COV-01 reads 1.0000 for ARM,
# ARM64, MIPS and PowerPC. Three of those four emit unmodelled pseudo-assembly
# calls on that same corpus:
#
#   arm64    __asm_movi                      1
#   mips     __asm_trunc.w.d                12
#   powerpc  __asm_rlwinm                   42
#
# Every one of those instructions has a function pointer in the table. The
# function emits a call.
#
# The probe (scripts/ci/pseudo_asm_probe.cpp) asks the translator instead: it
# translates each instruction into a throwaway function and uses the
# translator's own isPseudoAsmFunctionCall() predicate. Two `__asm_*` names are
# intended models rather than gaps -- __asm_hlt and __asm_rep_stosq_memset --
# and that list is read out of the product (isAsmIntrinsicName() in
# src/retdec/semantic_recovery_export.cpp), not kept here.
#
# Usage: bash scripts/ci/check_pseudo_asm.sh --capstone-prefix DIR
#            [--corpus DIR] [--jobs N] [--llvm-config PATH] [--top N]
#            [--arch LIST] [--arch-min "powerpc=1.0"] [--self-test] [--keep]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

JOBS="$(nproc 2>/dev/null || echo 4)"
CS_PREFIX=""
CORPUS=""
LLVM_CONFIG=""
TOP=10
ARCH=""
ARCH_MIN=""
SELF_TEST=0
KEEP=0
WORKDIR="${PSEUDO_WORKDIR:-}"

while [ $# -gt 0 ]; do
	case "$1" in
		--capstone-prefix) CS_PREFIX="$2"; shift 2 ;;
		--corpus)          CORPUS="$2"; shift 2 ;;
		--jobs)            JOBS="$2"; shift 2 ;;
		--llvm-config)     LLVM_CONFIG="$2"; shift 2 ;;
		--top)             TOP="$2"; shift 2 ;;
		--arch)            ARCH="$2"; shift 2 ;;
		--arch-min)        ARCH_MIN="$2"; shift 2 ;;
		--self-test)       SELF_TEST=1; shift ;;
		--keep)            KEEP=1; shift ;;
		*) echo "PSEUDO-01: unknown option: $1" >&2; exit 2 ;;
	esac
done

die() { echo "PSEUDO-01: FAIL $*" >&2; exit 1; }

[ -n "${CS_PREFIX}" ] || die "--capstone-prefix is required"
[ -f "${CS_PREFIX}/include/capstone/capstone.h" ] \
	|| die "no capstone headers under ${CS_PREFIX}"

if [ -z "${LLVM_CONFIG}" ]; then
	for c in llvm-config-20 llvm-config; do
		if command -v "$c" >/dev/null 2>&1; then LLVM_CONFIG="$(command -v "$c")"; break; fi
	done
fi
[ -n "${LLVM_CONFIG}" ] || die "no llvm-config found"

if [ -z "${WORKDIR}" ]; then
	WORKDIR="$(mktemp -d -t pseudo01-XXXXXX)"
	if [ "${KEEP}" = 0 ]; then
		trap 'rm -rf "${WORKDIR}"' EXIT
	fi
else
	mkdir -p "${WORKDIR}"
fi
mkdir -p "${WORKDIR}/obj"

LLVM_FLAGS="$("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' | tr '\n' ' ')"
INC="-Iinclude -Isrc -I${CS_PREFIX}/include -Ideps/rapidjson/include -DRAPIDJSON_HAS_STDSTRING=1"

srcs="${WORKDIR}/srcs.txt"
{
	find src/capstone2llvmir src/common src/config src/serdes -name '*.cpp'
	# src/utils minus the three that want generated headers or CUDA, none of
	# which the probe calls. Same exclusion list as C2L-01.
	find src/utils -name '*.cpp' \
		! -name 'binary_path.cpp' ! -name 'version.cpp' ! -name 'gpu_scanner*'
	find src/profiling -name '*.cpp'
	echo scripts/ci/pseudo_asm_probe.cpp
} | sort -u > "${srcs}"

n_srcs="$(wc -l < "${srcs}")"
[ "${n_srcs}" -ge 50 ] || die "expected at least 50 sources, found ${n_srcs}"
echo "PSEUDO-01: compiling ${n_srcs} translation units with ${JOBS} job(s)"

: > "${WORKDIR}/cc.err"
export WORKDIR
xargs -P "${JOBS}" -I{} sh -c '
	o="${WORKDIR}/obj/$(echo "$1" | tr "/" "_").o"
	g++ -std=c++17 -c -O0 '"${INC} ${LLVM_FLAGS}"' "$1" -o "$o" \
		2>>"${WORKDIR}/cc.err" || echo "FAILED $1" >> "${WORKDIR}/cc.err"
' _ {} < "${srcs}"

if grep -q '^FAILED ' "${WORKDIR}/cc.err"; then
	echo "PSEUDO-01: these do not compile:" >&2
	grep '^FAILED ' "${WORKDIR}/cc.err" >&2
	grep 'error:' "${WORKDIR}/cc.err" | head -20 >&2
	exit 1
fi

g++ -o "${WORKDIR}/probe" "${WORKDIR}"/obj/*.o \
	-L"${CS_PREFIX}/lib" -lcapstone \
	$("${LLVM_CONFIG}" --libs) $("${LLVM_CONFIG}" --system-libs) -pthread \
	2>"${WORKDIR}/link.err" \
	|| { grep -oE "undefined reference to .[^']*'" "${WORKDIR}/link.err" \
		| sort -u | head -20 >&2; die "link failed"; }

export LD_LIBRARY_PATH="${CS_PREFIX}/lib:${LD_LIBRARY_PATH:-}"

if [ "${SELF_TEST}" = 1 ]; then
	python3 scripts/ci/check_pseudo_asm.py --probe "${WORKDIR}/probe" --self-test
fi

if [ -z "${CORPUS}" ]; then
	[ "${SELF_TEST}" = 1 ] || die "--corpus is required unless --self-test is given"
	exit 0
fi

args=(--corpus "${CORPUS}" --probe "${WORKDIR}/probe" --top "${TOP}")
[ -n "${ARCH}" ] && args+=(--arch "${ARCH}")
[ -n "${ARCH_MIN}" ] && args+=(--arch-min "${ARCH_MIN}")
python3 scripts/ci/check_pseudo_asm.py "${args[@]}"
