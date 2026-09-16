#!/usr/bin/env bash
# FLAG-01 — compare the x86 translator's arithmetic and condition codes
# against the CPU this runs on.
#
# Why this exists
# ---------------
# Every other check on this branch asks whether an instruction is TRANSLATED.
# None of them asks whether it is translated CORRECTLY, because the tests that
# would answer that were written by whoever wrote the translator, from the
# same reading of the manual. A test written from a misunderstanding passes.
#
# So this does not read the manual. It executes each instruction on the host
# CPU with random operands, captures the result and all six arithmetic flags,
# runs the translator's own IR over the same inputs in RetDec's emulator, and
# compares. Two real bugs came out of the first run:
#
#   * SBB applied its incoming carry twice -- once folded into the operand and
#     once inside the flag helpers, which load CF themselves when it is not
#     passed. 126 of the 127 disagreements were SBB; ADC, which passes its
#     carry explicitly, had none.
#   * NEG's overflow flag was hardcoded to zero. It is set for exactly one
#     input, the minimum signed value.
#
# and a third, on ARM, from the shared helper SBB was misusing.
#
# Two comparisons run:
#
#   arithmetic  34 instruction forms x N random operand pairs. Flags the
#               architecture leaves UNDEFINED are excluded per instruction --
#               IMUL's SF/ZF/AF/PF, the logicals' AF, OF for shifts by other
#               than one -- because comparing against an undefined flag is
#               comparing against whatever this CPU happened to leave behind.
#
#   wide        MUL, IMUL, DIV, IDIV, SHLD, SHRD and the single-operand
#               shifts and rotates at 8, 16 and 32 bits -- everything whose
#               answer does not fit in one register and six flags. Both RAX
#               and RDX are compared in full, before and after, so a
#               translator that zeroes what it should merge, or merges what
#               it should zero, is a mismatch. Four bugs came out of the
#               first run: IDIV zero-extended its divisor, one-operand IMUL
#               got its overflow test wrong, SHLD and SHRD masked the count
#               by the processor mode instead of the operand size, and none
#               of the nine shift forms wrote its destination when the count
#               masked to zero -- which on x86-64 is how the upper half of a
#               32-bit destination gets cleared.
#
#   condition   every SETcc against every one of the 64 combinations of the
#               six flags. Exhaustive, not sampled: 16 x 64 = 1024 cases. Jcc,
#               SETcc and CMOVcc all go through the same sixteen generateCc*
#               helpers, so this covers every conditional in x86 output.
#
# Requires an x86-64 host: it runs the instructions. Skips cleanly elsewhere.
#
# Usage: bash scripts/ci/check_x86_flags.sh --capstone-prefix DIR
#            [--rows N] [--jobs N] [--llvm-config PATH] [--self-test] [--keep]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

JOBS="$(nproc 2>/dev/null || echo 4)"
CS_PREFIX=""
LLVM_CONFIG=""
ROWS=400
KEEP=0
SELF_TEST=0
WORKDIR="${FLAG_WORKDIR:-}"

die() { echo "FLAG-01: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--capstone-prefix) CS_PREFIX="$2"; shift 2 ;;
		--rows)            ROWS="$2"; shift 2 ;;
		--jobs)            JOBS="$2"; shift 2 ;;
		--llvm-config)     LLVM_CONFIG="$2"; shift 2 ;;
		--self-test)       SELF_TEST=1; shift ;;
		--keep)            KEEP=1; shift ;;
		*) die "unknown option: $1" ;;
	esac
done

arch="$(uname -m)"
if [ "${arch}" != "x86_64" ]; then
	echo "FLAG-01: SKIP -- needs an x86-64 host to execute the instructions, this is ${arch}"
	exit 0
fi

[ -n "${CS_PREFIX}" ] || die "--capstone-prefix is required"
[ -d "${CS_PREFIX}/include" ] || die "no include/ under ${CS_PREFIX}"

if [ -z "${LLVM_CONFIG}" ]; then
	for c in llvm-config-20 llvm-config-19 llvm-config; do
		if command -v "$c" >/dev/null 2>&1; then LLVM_CONFIG="$c"; break; fi
	done
fi
[ -n "${LLVM_CONFIG}" ] || die "no llvm-config found; pass --llvm-config"

if [ -z "${WORKDIR}" ]; then
	WORKDIR="$(mktemp -d)"
	if [ "${KEEP}" -eq 0 ]; then trap 'rm -rf "${WORKDIR}"' EXIT; fi
fi
mkdir -p "${WORKDIR}/obj"

LLVM_FLAGS="$("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' | tr '\n' ' ')"
INC="-Iinclude -Isrc -I${CS_PREFIX}/include -Ideps/rapidjson/include -DRAPIDJSON_HAS_STDSTRING=1"

srcs="${WORKDIR}/srcs.txt"
{
	find src/capstone2llvmir src/common src/config src/serdes src/llvmir-emul -name '*.cpp'
	find src/utils -name '*.cpp' \
		! -name 'binary_path.cpp' ! -name 'version.cpp' ! -name 'gpu_scanner*'
	find src/profiling -name '*.cpp'
} | sort -u > "${srcs}"

n_srcs="$(wc -l < "${srcs}")"
[ "${n_srcs}" -ge 50 ] || die "expected at least 50 sources, found ${n_srcs}"
echo "FLAG-01: compiling ${n_srcs} translation units with ${JOBS} job(s)"

: > "${WORKDIR}/cc.err"
export WORKDIR
xargs -P "${JOBS}" -I{} sh -c '
	o="${WORKDIR}/obj/$(echo "$1" | tr "/" "_").o"
	g++ -std=c++17 -c -O1 '"${INC} ${LLVM_FLAGS}"' "$1" -o "$o" \
		2>>"${WORKDIR}/cc.err" || echo "FAILED $1" >> "${WORKDIR}/cc.err"
' _ {} < "${srcs}"

if grep -q '^FAILED ' "${WORKDIR}/cc.err"; then
	grep '^FAILED ' "${WORKDIR}/cc.err" | head -5
	sed -n '1,40p' "${WORKDIR}/cc.err"
	die "compilation failed"
fi

link() {
	g++ -std=c++17 -O1 ${INC} ${LLVM_FLAGS} "$1" "${WORKDIR}"/obj/*.o -o "$2" \
		-L"${CS_PREFIX}/lib" -lcapstone \
		$("${LLVM_CONFIG}" --libs) $("${LLVM_CONFIG}" --system-libs) -pthread \
		2>"${WORKDIR}/link.err" || { sed -n '1,30p' "${WORKDIR}/link.err"; die "link failed: $1"; }
}

gcc -O0 -o "${WORKDIR}/flag_oracle" scripts/ci/x86_flag_oracle.c || die "oracle build failed"
gcc -O0 -o "${WORKDIR}/cc_oracle"   scripts/ci/x86_cc_oracle.c   || die "cc oracle build failed"
gcc -O0 -o "${WORKDIR}/wide_oracle" scripts/ci/x86_wide_oracle.c || die "wide oracle build failed"
link scripts/ci/x86_flag_compare.cpp "${WORKDIR}/flag_compare"
link scripts/ci/x86_cc_compare.cpp   "${WORKDIR}/cc_compare"
link scripts/ci/x86_wide_compare.cpp "${WORKDIR}/wide_compare"

rc=0

# A comparison that cannot report a difference proves nothing. Corrupt one
# expected flag and check that exactly one mismatch comes back -- the same
# role the md5 guard plays in a mutation driver.
if [ "${SELF_TEST}" -eq 1 ]; then
	"${WORKDIR}/flag_oracle" 20 > "${WORKDIR}/rows.txt"
	awk -F'|' 'BEGIN{OFS="|"} NR==2 && NF==12 {$10 = ($10=="1" ? "0" : "1")} {print}' \
		"${WORKDIR}/rows.txt" > "${WORKDIR}/rows.bad"
	cmp -s "${WORKDIR}/rows.txt" "${WORKDIR}/rows.bad" \
		&& die "self-test: corrupting a row changed nothing"
	st="$(cd "${WORKDIR}" && ./flag_compare rows.bad | tail -1)"
	case "${st}" in
		*", 1 mismatches"*) echo "FLAG-01: self-test ok -- ${st}" ;;
		*) die "self-test: expected exactly 1 mismatch, got: ${st}" ;;
	esac

	# The wide comparison checks two result registers as well as the flags,
	# so its self-test corrupts a RESULT: that is the half the flag
	# comparison cannot reach, and IDIV's sign bug showed up there and
	# nowhere else.
	"${WORKDIR}/wide_oracle" 20 > "${WORKDIR}/wrows.txt"
	# The last digit is rewritten as TEXT, not incremented. awk keeps numbers
	# in doubles, so `$6 + 1` on a 64-bit result yields 1.47884e+19, which
	# does not parse back as an integer -- the row is silently dropped and
	# the comparison reports one FEWER comparison and no mismatch. A
	# self-test that quietly removes its own evidence is worse than none.
	awk -F'|' 'BEGIN{OFS="|"} NR==2 && NF==13 {
			last = substr($6, length($6));
			$6 = substr($6, 1, length($6) - 1) (last == "0" ? "1" : "0")
		} {print}' \
		"${WORKDIR}/wrows.txt" > "${WORKDIR}/wrows.bad"
	cmp -s "${WORKDIR}/wrows.txt" "${WORKDIR}/wrows.bad" \
		&& die "self-test: corrupting a wide row changed nothing"
	st="$(cd "${WORKDIR}" && ./wide_compare wrows.bad | tail -1)"
	case "${st}" in
		*", 1 mismatches"*) echo "FLAG-01: wide self-test ok -- ${st}" ;;
		*) die "wide self-test: expected exactly 1 mismatch, got: ${st}" ;;
	esac
fi

"${WORKDIR}/flag_oracle" "${ROWS}" > "${WORKDIR}/rows.txt"
out="$(cd "${WORKDIR}" && ./flag_compare rows.txt)"
echo "${out}" | tail -20
if ! echo "${out}" | grep -qE ', 0 mismatches'; then
	echo "FLAG-01: FAIL arithmetic flags disagree with the hardware" >&2
	rc=1
fi

"${WORKDIR}/wide_oracle" "${ROWS}" > "${WORKDIR}/wrows.txt"
out="$(cd "${WORKDIR}" && ./wide_compare wrows.txt)"
echo "${out}" | tail -20
if ! echo "${out}" | grep -qE ', 0 mismatches'; then
	echo "FLAG-01: FAIL wide results or flags disagree with the hardware" >&2
	rc=1
fi

"${WORKDIR}/cc_oracle" > "${WORKDIR}/rows.txt"
out="$(cd "${WORKDIR}" && ./cc_compare rows.txt)"
echo "${out}" | tail -20
if ! echo "${out}" | grep -qE ', 0 mismatches'; then
	echo "FLAG-01: FAIL condition codes disagree with the hardware" >&2
	rc=1
fi

[ "${rc}" -eq 0 ] && echo "FLAG-01: OK"
exit "${rc}"
