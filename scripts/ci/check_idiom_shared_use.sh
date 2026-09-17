#!/usr/bin/env bash
# IDIOM-USE-01 -- an idiom rewrite must not corrupt a use it did not match.
#
# IdiomsAbstract::eraseInstFromBasicBlock removes the inner nodes of a matched
# idiom tree. It used to do that with replaceAllUsesWith(UndefValue) and an
# unconditional erase, which is right when the idiom's own root was the node's
# only reader and a silent miscompile when it was not:
#
#     %y = lshr i32 %x, 31
#     %z = xor  i32 %y, 1     ->  rewritten to  X >= 0
#     %w = add  i32 %y, 5     ->  becomes  add i32 undef, 5
#
# The pass reports success, the IR verifies, and the emitted C is wrong. 129
# call sites went through that helper.
#
# This builds each shape twice, runs the real pass on one, and evaluates both
# over a domain of inputs with LLVM's own constant folder, requiring the same
# answer. It reports per case whether the pass changed anything, so a case the
# pass declines is visible as "measured nothing" rather than counted as a pass.
#
# The idiom sources are compiled into a shared object with
# --unresolved-symbols=ignore-all rather than linked against the whole of
# bin2llvmir, which would pull in fileformat, loader and the rest of the tree.
#
# Usage: bash scripts/ci/check_idiom_shared_use.sh [--llvm-config PATH] [--keep]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

LLVM_CONFIG=""
KEEP=0
WORKDIR="${IDIOM_USE_WORKDIR:-}"

while [ $# -gt 0 ]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--keep) KEEP=1; shift ;;
		--workdir) WORKDIR="$2"; shift 2 ;;
		--self-test) shift ;;   # the negative control always runs
		*) echo "IDIOM-USE-01: unknown argument $1" >&2; exit 2 ;;
	esac
done

die() { echo "IDIOM-USE-01: FAIL $*" >&2; exit 1; }

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

for f in src/bin2llvmir/optimizations/idioms/*.cpp; do
	o="${WORKDIR}/$(basename "$f" .cpp).o"
	g++ -std=c++17 -O0 -w -fPIC ${INC} ${LLVM_FLAGS} -c "$f" -o "$o" \
		2>"${WORKDIR}/cc.err" \
		|| { sed -n '1,20p' "${WORKDIR}/cc.err" >&2; die "$f did not compile"; }
done

g++ -shared -o "${WORKDIR}/libid.so" "${WORKDIR}"/*.o \
	-Wl,--unresolved-symbols=ignore-all 2>"${WORKDIR}/so.err" \
	|| { sed -n '1,20p' "${WORKDIR}/so.err" >&2; die "could not build the idiom library"; }

g++ -std=c++17 -O0 -w ${INC} ${LLVM_FLAGS} \
	scripts/ci/idiom_shared_use_check.cpp -o "${WORKDIR}/probe" \
	-L"${WORKDIR}" -lid -Wl,-rpath,"${WORKDIR}" \
	-Wl,--unresolved-symbols=ignore-all \
	$("${LLVM_CONFIG}" --libs) $("${LLVM_CONFIG}" --system-libs) -pthread \
	2>"${WORKDIR}/link.err" \
	|| { sed -n '1,20p' "${WORKDIR}/link.err" >&2; die "IDIOM-USE-01 link failed"; }

set +e
"${WORKDIR}/probe" --self-test > "${WORKDIR}/run.log" 2>&1
status=$?
set -e

cat "${WORKDIR}/run.log"

if grep -q "measured nothing" "${WORKDIR}/run.log" \
		&& ! grep -q "self-test ok" "${WORKDIR}/run.log"; then
	die "a case the pass declined was counted as a pass"
fi

if [ "${status}" != 0 ]; then
	die "an idiom rewrite changed what a function computes"
fi

echo "IDIOM-USE-01: OK idiom rewrites preserve uses they did not match (LLVM ${LLVM_MAJOR})"
