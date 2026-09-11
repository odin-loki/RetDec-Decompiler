#!/usr/bin/env bash
# build_multiarch_corpus.sh — build the algorithm-recovery sources for the four
# non-x86 architectures RetDec translates.
#
# Why this exists
# ---------------
# Every end-to-end gate in this repository measures one architecture. All 216
# binaries in tests/algorithm_recovery/corpus are x86-64, so CC-01, DET-01 and
# the F1 recovery gate say nothing about ARM, ARM64, MIPS or PowerPC -- the
# four other architectures src/capstone2llvmir translates. C2L-01 checks their
# instruction semantics; nothing checks what the decompiler does with a whole
# binary.
#
# The binaries are built, not committed: they come from the same sources as the
# x86-64 corpus, and four more architectures of them is several hundred files
# that would go stale the moment a source changed.
#
# Static, and here is why it matters for what is being measured: a dynamically
# linked cross binary leaves the algorithm in a PLT stub and the decompiler
# with almost nothing to recover, so the run would look like a pass while
# testing nothing. Static linking puts the real code in the file.
#
# gcc only. There is no cross-clang in the distribution's toolchain packages,
# so the compiler dimension the x86-64 corpus has (gcc and clang) is not
# available here; that is a gap in this corpus, not a choice.
#
# Usage: bash scripts/build_multiarch_corpus.sh [--out DIR] [--opts "O0 O2"]
#                                               [--arch LIST] [--quiet]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/tests/algorithm_recovery/sources"
OUT="${ROOT}/tests/algorithm_recovery/corpus-multiarch"
OPTS="O0"
QUIET=0

# The four architectures src/capstone2llvmir has a translator for, with the
# triple that builds each. 32-bit ARM is built for the hard-float EABI because
# that is what the distribution ships; MIPS and PowerPC are big-endian here,
# which is the side of those two that x86-64 testing never exercises at all.
ARCHES="arm64:aarch64-linux-gnu arm:arm-linux-gnueabihf mips:mips-linux-gnu powerpc:powerpc-linux-gnu"

while [ $# -gt 0 ]; do
	case "$1" in
		--out)   OUT="$2"; shift 2 ;;
		--opts)  OPTS="$2"; shift 2 ;;
		--arch)  ARCHES="$2"; shift 2 ;;
		--quiet) QUIET=1; shift ;;
		*) echo "build_multiarch_corpus: unknown option: $1" >&2; exit 2 ;;
	esac
done

say() { [ "${QUIET}" = 1 ] || echo "$@"; }

missing=""
for pair in ${ARCHES}; do
	triple="${pair##*:}"
	command -v "${triple}-gcc" >/dev/null 2>&1 || missing="${missing} ${triple}-gcc"
done
if [ -n "${missing}" ]; then
	echo "build_multiarch_corpus: missing cross compilers:${missing}" >&2
	echo "  apt-get install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf \\" >&2
	echo "                  gcc-mips-linux-gnu gcc-powerpc-linux-gnu \\" >&2
	echo "                  libc6-dev-arm64-cross libc6-dev-armhf-cross \\" >&2
	echo "                  libc6-dev-mips-cross libc6-dev-powerpc-cross" >&2
	exit 1
fi

mkdir -p "${OUT}"

built=0
failed=0
: > "${OUT}/.build.log"
for cfile in $(find "${SRC}" -name '*.c' \
		-not -path '*/negative*' -not -path '*/adversarial/*' \
		-not -path '*/third_party/*' | sort); do
	rel="${cfile#${SRC}/}"
	stem="$(echo "${rel%.c}" | tr '/' '_')"
	link=""
	grep -q 'pthread' "${cfile}" && link="-pthread"
	for pair in ${ARCHES}; do
		arch="${pair%%:*}"; triple="${pair##*:}"
		for opt in ${OPTS}; do
			name="${stem}-${arch}-gcc-${opt}"
			if "${triple}-gcc" "-${opt}" -std=c11 -static \
					-o "${OUT}/${name}" "${cfile}" ${link} \
					>>"${OUT}/.build.log" 2>&1; then
				built=$((built + 1))
			else
				failed=$((failed + 1))
				echo "FAILED ${name}" >> "${OUT}/.build.log"
			fi
		done
	done
done

say "build_multiarch_corpus: ${built} binaries in ${OUT} (${failed} failed)"
if [ "${failed}" != 0 ]; then
	grep '^FAILED ' "${OUT}/.build.log" | head -10 >&2
	exit 1
fi
