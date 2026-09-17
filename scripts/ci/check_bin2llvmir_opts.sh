#!/usr/bin/env bash
# OPT-01 — do the bin2llvmir peepholes still compute the same number?
#
# Why this exists
# ---------------
# bin2llvmir is where RetDec rewrites the lifted IR, and until this script it
# had no gate at all. tests/bin2llvmir/CMakeLists.txt builds no tests for
# strength_reduction, redundant_load_store, inst_opt_rda or inst_opt_ext, and
# the tests it does build for inst_opt compare IR text: they answer "did the
# rewrite produce the instruction I wrote down", not "do the two programs
# compute the same thing".
#
# Those are different questions, and the gap between them was holding real
# defects. `and i1 x, y` was rewritten to `icmp eq i1 x, y`, which is the
# opposite answer when x and y are both zero; the test that covered it chose
# `and i1 %a, 1`, the one input pattern where the two agree, so it had never
# been able to fail. `lshr (shl x, N), N` masked the complement of the bits it
# should have kept, and the file's own header comment documented the same
# inversion, so reading either one confirmed the other.
#
# What this does instead
# ----------------------
# It takes the module before the rewrite and after it, evaluates BOTH over a
# domain of inputs, and requires the same answer every time. The evaluator is
# LLVM's own constant folder, which knows nothing about RetDec and cannot be
# talked into agreeing. See scripts/ci/inst_opt_semantics_check.cpp, which also
# states plainly what it cannot see -- poison, control flow, partial overwrites
# -- so that nobody reads a pass here as more than it is.
#
# About the link
# --------------
# The passes are compiled into a shared object with
# --unresolved-symbols=ignore-all rather than linked against the whole of
# bin2llvmir, which transitively pulls in fileformat, loader, demangler and the
# rest of the tree. Nothing in the corpus reaches an unresolved symbol; if a
# future case does, it dies loudly at the call rather than answering wrongly.
#
# Usage: bash scripts/ci/check_bin2llvmir_opts.sh [--llvm-config PATH] [--keep]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

LLVM_CONFIG=""
KEEP=0
WORKDIR="${OPT_WORKDIR:-}"

while [ $# -gt 0 ]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--keep) KEEP=1; shift ;;
		--workdir) WORKDIR="$2"; shift 2 ;;
		--self-test) shift ;;   # always runs; accepted for symmetry
		*) echo "OPT-01: unknown argument $1" >&2; exit 2 ;;
	esac
done

die() { echo "OPT-01: FAIL $*" >&2; exit 1; }

if [ -z "${LLVM_CONFIG}" ]; then
	# Highest version available, the same way check_llvmir2hll_tests.sh and the
	# other LLVM checks here pick theirs. A hardcoded list picks whatever was
	# current when it was written: this one said `llvm-config-20 llvm-config-21
	# llvm-config`, which would have taken 20 over 21 and fallen back to the
	# runner's default -- 18 on ubuntu-24.04 -- when neither was installed.
	for c in $(compgen -c llvm-config 2>/dev/null | sort -Vru) llvm-config; do
		command -v "$c" >/dev/null 2>&1 && { LLVM_CONFIG="$c"; break; }
	done
fi
[ -n "${LLVM_CONFIG}" ] || die "no llvm-config found -- install llvm-20-dev"
LLVM_MAJOR="$("${LLVM_CONFIG}" --version | cut -d. -f1)"
[ "${LLVM_MAJOR}" -ge 20 ] \
	|| die "${LLVM_CONFIG} is LLVM ${LLVM_MAJOR}; this needs 20 or newer"

if [ -z "${WORKDIR}" ]; then
	WORKDIR="$(mktemp -d)"
	[ "${KEEP}" = 1 ] || trap 'rm -rf "${WORKDIR}"' EXIT
fi
mkdir -p "${WORKDIR}"

LLVM_FLAGS="$("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' | tr '\n' ' ')"
INC="-Iinclude -Isrc -Ideps/rapidjson/include -Ideps/elfio/include -DRAPIDJSON_HAS_STDSTRING=1"

PASS_SRCS="
	src/bin2llvmir/optimizations/inst_opt/inst_opt.cpp
	src/bin2llvmir/optimizations/inst_opt/inst_opt_ext.cpp
	src/bin2llvmir/optimizations/strength_reduction/strength_reduction.cpp
	src/bin2llvmir/optimizations/redundant_load_store/redundant_load_store.cpp
	src/bin2llvmir/utils/llvm.cpp
	src/bin2llvmir/providers/abi/abi.cpp
"

for f in ${PASS_SRCS}; do
	[ -f "$f" ] || die "missing source $f"
	o="${WORKDIR}/$(basename "$f" .cpp).o"
	g++ -std=c++17 -O0 -w -fPIC ${INC} ${LLVM_FLAGS} -c "$f" -o "$o" \
		2>"${WORKDIR}/cc.err" \
		|| { sed -n '1,20p' "${WORKDIR}/cc.err" >&2; die "$f did not compile"; }
done

g++ -shared -o "${WORKDIR}/libopt.so" "${WORKDIR}"/*.o \
	-Wl,--unresolved-symbols=ignore-all \
	2>"${WORKDIR}/so.err" \
	|| { sed -n '1,20p' "${WORKDIR}/so.err" >&2; die "could not build the pass library"; }

g++ -std=c++17 -O0 -w ${INC} ${LLVM_FLAGS} \
	scripts/ci/inst_opt_semantics_check.cpp \
	-o "${WORKDIR}/optck" \
	-L"${WORKDIR}" -lopt -Wl,-rpath,"${WORKDIR}" \
	-Wl,--unresolved-symbols=ignore-all \
	$("${LLVM_CONFIG}" --libs) $("${LLVM_CONFIG}" --system-libs) -pthread \
	2>"${WORKDIR}/link.err" \
	|| { sed -n '1,20p' "${WORKDIR}/link.err" >&2; die "OPT-01 link failed"; }

set +e
"${WORKDIR}/optck" --self-test > "${WORKDIR}/run.log" 2>&1
status=$?
set -e

if [ "${status}" != 0 ]; then
	grep -E 'MISMATCH|FAIL|self-test' "${WORKDIR}/run.log" | head -40 >&2
	die "a bin2llvmir rewrite changed what a function computes"
fi

grep -E 'self-test ok|examined|preserved' "${WORKDIR}/run.log"
echo "OPT-01: OK against LLVM ${LLVM_MAJOR}"
