#!/usr/bin/env bash
# L2HW-01 -- compile src/llvmir2hll with warnings on, and keep the list short.
#
# Why this exists
# ---------------
# Nothing compiles src/llvmir2hll with warnings enabled anywhere that runs
# outside the pinned-LLVM build. L2H-01 compiles it with `-w`, because it is
# there to run the tests; standalone_check.sh covers only the modules that need
# no third-party library, and llvmir2hll needs LLVM. So `-Wunused-function` --
# the warning that says "this code is never called" -- had never been read for
# 304 translation units.
#
# What it was hiding, in one file: a 213-line `FuncRewriter` class that was
# never constructed and had not been reviewed either (two of its loop advances
# were `stmt = stmt;`, one insert was `newIf->prependStatement(newIf)`, and it
# carried two members for the same function), plus two helpers written for it
# that nothing called. 270 lines.
#
# The density is low -- six warnings over the whole of src/llvmir2hll once that
# file was cleaned up -- so this is a gate rather than a wish. Each of the six
# is listed in EXPECTED below with a reason, and the list is checked in both
# directions: a warning that is not on it fails this check, and an entry that
# stops warning fails it as a stale excuse.
#
# Usage: bash scripts/ci/check_llvmir2hll_warnings.sh [--llvm-config PATH]
#                                                     [--jobs N] [--workdir DIR]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

LLVM_CONFIG=""
JOBS="$(nproc 2>/dev/null || echo 2)"
WORKDIR="${L2HW_WORKDIR:-}"

while [ $# -gt 0 ]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--jobs) JOBS="$2"; shift 2 ;;
		--workdir) WORKDIR="$2"; shift 2 ;;
		--self-test) shift ;;   # the two-way list check always runs
		*) echo "L2HW-01: unknown argument $1" >&2; exit 2 ;;
	esac
done

die() { echo "L2HW-01: FAIL $*" >&2; exit 1; }

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

if [ -z "${WORKDIR}" ]; then
	WORKDIR="$(mktemp -d)"
	trap 'rm -rf "${WORKDIR}"' EXIT
fi
mkdir -p "${WORKDIR}"

# file|symbol|reason. Every one of these is a table of symbolic names written
# for an API and never added to the function-parameter map. Wiring them is not
# a matter of adding a line: the map is keyed by function name and parameter
# position only, and each of these needs something the map cannot say.
EXPECTED="
src/llvmir2hll/semantics/semantics/libc_semantics/get_symbolic_names_for_param.cpp|getSymbolicNamesForBSDSignals|the BSD and Linux signal numberings differ and the map has no platform key, so wiring it would give one of the two the wrong names
src/llvmir2hll/semantics/semantics/libc_semantics/get_symbolic_names_for_param.cpp|getSymbolicNamesForTCPOptions|setsockopt's optname meaning depends on its level argument, which a function-plus-position map cannot express
src/llvmir2hll/semantics/semantics/libc_semantics/get_symbolic_names_for_param.cpp|getSymbolicNamesForKqueueFilter|kqueue is BSD/macOS; sys/event.h is not present on this container, so the table cannot be checked against a real header the way check_libc_arity.py checks the others
src/llvmir2hll/semantics/semantics/libc_semantics/get_symbolic_names_for_param.cpp|getSymbolicNamesForKqueueFlags|same as the filter table above
src/llvmir2hll/semantics/semantics/libc_semantics/get_symbolic_names_for_param.cpp|getSymbolicNamesForDispatchQueueAttr|libdispatch is macOS; no header here to check it against
src/llvmir2hll/semantics/semantics/win_api_semantics/get_symbolic_names_for_param.cpp|getSymbolicNamesForWin32Error|a RETURN value, not a parameter; the map is indexed by parameter position and has no slot for one
"

LLVM_FLAGS="$("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' | tr '\n' ' ')"
INC="-Iinclude -Isrc -Ideps/rapidjson/include -DRAPIDJSON_HAS_STDSTRING=1"

mapfile -t SRCS < <(find src/llvmir2hll -name '*.cpp' | sort)
[ "${#SRCS[@]}" -ge 250 ] \
	|| die "only ${#SRCS[@]} sources under src/llvmir2hll; the tree does not look complete"

echo "L2HW-01: compiling ${#SRCS[@]} translation units with warnings on"

# -Wno-unused-parameter: the visitor interfaces this tree is built on take
# parameters their overrides do not read, by design, and there are hundreds.
warn_one() {
	f="$1"
	g++ -std=c++17 -O0 -c -Wall -Wextra -Wno-unused-parameter \
		${L2HW_INC} ${L2HW_LF} "$f" -o /dev/null \
		2> "${L2HW_OUT}/$(echo "$f" | tr '/' '_').txt" || true
}
export -f warn_one
export L2HW_OUT="${WORKDIR}" L2HW_INC="${INC}" L2HW_LF="${LLVM_FLAGS}"

printf '%s\n' "${SRCS[@]}" | xargs -P "${JOBS}" -I{} bash -c 'warn_one "$@"' _ {}

# A compile that FAILED is not a warning result; say so rather than reading an
# empty warning list as a clean one.
if grep -l -m1 'error:' "${WORKDIR}"/*.txt >/dev/null 2>&1; then
	grep -h -m 5 'error:' "${WORKDIR}"/*.txt >&2
	die "a source under src/llvmir2hll did not compile"
fi

ALL="$(cat "${WORKDIR}"/*.txt 2>/dev/null | grep 'warning:' || true)"

unexpected=0
while IFS= read -r line; do
	[ -n "${line}" ] || continue
	file="${line%%:*}"
	matched=""
	while IFS='|' read -r efile esym _ereason; do
		[ -n "${efile}" ] || continue
		if [ "${efile}" = "${file}" ] && [[ "${line}" == *"${esym}"* ]]; then
			matched=1
		fi
	done <<< "${EXPECTED}"
	if [ -z "${matched}" ]; then
		echo "${line}" >&2
		unexpected=$((unexpected + 1))
	fi
done <<< "${ALL}"

if [ "${unexpected}" -gt 0 ]; then
	die "${unexpected} warning(s) not in EXPECTED. Fix them, or add them there with a reason -- and a reason has to say why the code should stay, not that nobody has got to it"
fi

stale=""
while IFS='|' read -r efile esym _ereason; do
	[ -n "${efile}" ] || continue
	printf '%s' "${ALL}" | grep -qF "${esym}" || stale="${stale} ${esym}"
done <<< "${EXPECTED}"
if [ -n "${stale}" ]; then
	die "EXPECTED names symbol(s) that no longer warn; remove them:${stale}"
fi

TOTAL="$(printf '%s' "${ALL}" | grep -c 'warning:' || true)"
echo "L2HW-01: OK ${#SRCS[@]} translation units, ${TOTAL} warning(s), all recorded"
