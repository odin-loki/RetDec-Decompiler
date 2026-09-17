#!/usr/bin/env bash
# IDIOM-PHI-01 -- keep the "no exchanger fires on a PHI" claim measured.
#
# idioms_analysis.cpp's PHI fix-up carries a comment stating that none of the
# registered per-basic-block exchangers returns non-null for a PHI input, and a
# count of how many pairs were tried to establish it.  That claim is the reason
# the fix-up is described as a trap rather than a live defect, so it has to stay
# true rather than stay written down.
#
# This builds scripts/ci/idiom_phi_reach_probe.cpp against the real exchangers,
# runs it, and requires the measured count of PHI-accepting exchangers to match
# the number the comment claims.  It also requires the probe's negative control
# to fire, because a probe that calls nothing reports zero just as convincingly
# as one that calls everything.
#
# The idiom sources are compiled into a shared object with
# --unresolved-symbols=ignore-all rather than linked against the whole of
# bin2llvmir, which would pull in fileformat, loader and the rest of the tree.
#
# Usage: bash scripts/ci/check_idiom_phi_reach.sh [--llvm-config PATH] [--keep]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

LLVM_CONFIG=""
KEEP=0
WORKDIR="${IDIOM_PHI_WORKDIR:-}"

while [ $# -gt 0 ]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--keep) KEEP=1; shift ;;
		--workdir) WORKDIR="$2"; shift 2 ;;
		--self-test) shift ;;   # the negative control always runs
		*) echo "IDIOM-PHI-01: unknown argument $1" >&2; exit 2 ;;
	esac
done

die() { echo "IDIOM-PHI-01: FAIL $*" >&2; exit 1; }

if [ -z "${LLVM_CONFIG}" ]; then
	for c in llvm-config-20 llvm-config-21 llvm-config; do
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

ANALYSIS="src/bin2llvmir/optimizations/idioms/idioms_analysis.cpp"

# Every exchanger the dispatcher registers must be one the probe calls.  A
# probe that silently covers half the set reports "none of them" just as
# cleanly as one that covers all of them.
missing=""
while read -r name; do
	[ -n "${name}" ] || continue
	grep -q "P(${name})" scripts/ci/idiom_phi_reach_probe.cpp || missing="${missing} ${name}"
done < <(grep -oE "analyse\(bb, &Idioms[A-Za-z]+::[a-zA-Z0-9_]+" "${ANALYSIS}" \
	| sed 's/.*:://' | sort -u)
if [ -n "${missing}" ]; then
	die "the probe does not call these registered exchangers:${missing}"
fi

# The number the comment claims.
CLAIMED="$(grep -oE 'all [0-9]+ were called on PHIs of [a-z]+ types, [0-9]+ pairs, and none' \
	"${ANALYSIS}" | grep -oE '^all [0-9]+' | grep -oE '[0-9]+' || true)"
[ -n "${CLAIMED}" ] \
	|| die "could not find the exchanger-count claim in ${ANALYSIS}"

REGISTERED="$(grep -oE "analyse\(bb, &Idioms[A-Za-z]+::[a-zA-Z0-9_]+" "${ANALYSIS}" \
	| sed 's/.*:://' | sort -u | wc -l)"
[ "${CLAIMED}" = "${REGISTERED}" ] \
	|| die "${ANALYSIS} claims ${CLAIMED} exchangers; ${REGISTERED} are registered"

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
	scripts/ci/idiom_phi_reach_probe.cpp -o "${WORKDIR}/probe" \
	-L"${WORKDIR}" -lid -Wl,-rpath,"${WORKDIR}" \
	-Wl,--unresolved-symbols=ignore-all \
	$("${LLVM_CONFIG}" --libs) $("${LLVM_CONFIG}" --system-libs) -pthread \
	2>"${WORKDIR}/link.err" \
	|| { sed -n '1,20p' "${WORKDIR}/link.err" >&2; die "IDIOM-PHI-01 link failed"; }

set +e
"${WORKDIR}/probe" > "${WORKDIR}/run.log" 2>&1
status=$?
set -e

cat "${WORKDIR}/run.log"

if [ "${status}" = 2 ]; then
	die "the probe's negative control did not fire, so it measured nothing"
fi

FIRED="$(grep -oE '; [0-9]+ fired on a PHI' "${WORKDIR}/run.log" \
	| grep -oE '[0-9]+' || true)"
[ -n "${FIRED}" ] || die "the probe did not report a count"

if [ "${FIRED}" != 0 ]; then
	echo "IDIOM-PHI-01: ${FIRED} exchanger(s) now fire on a PHI." >&2
	echo "       The fix-up in ${ANALYSIS} handles that correctly, but the" >&2
	echo "       comment there says none does.  Update the comment and this" >&2
	echo "       check, and add a test covering the exchanger that fires." >&2
	exit 1
fi

echo "IDIOM-PHI-01: OK ${REGISTERED} registered exchangers, none accepts a PHI (LLVM ${LLVM_MAJOR})"
