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
# Dynamically linked by default, with the same flags the x86-64 corpus is built
# with (scripts/decompiler/build_corpus_fixtures.sh: -O0 -fno-inline
# -fno-builtin, no -static). That is the point: ARCH-01 is asking whether the
# other four architectures reach x86-64's result, and it can only ask that if
# it hands them the same kind of binary.
#
# This defaulted to -static, on the reasoning that "a dynamically linked cross
# binary leaves the algorithm in a PLT stub and the decompiler with almost
# nothing to recover". That reasoning is wrong, and checking it takes one
# command: `nm` on a dynamically linked cross build finds bubble_sort and main
# in the binary. Only the libc calls go through the PLT, exactly as in the
# x86-64 corpus that the F1 gate scores every run. What -static actually added
# was all of glibc: 400-700 KB instead of 8-70 KB, and ARM, MIPS and PowerPC
# all ran the entire pipeline and then hit ARCH-01's 60-second timeout at
# "Disassembly generation", 58 seconds in. The gate read 0/40 for a reason
# that was about the corpus, not the decompiler.
#
# --link static is still here, and is worth running deliberately: the static
# set drags in glibc's hand-written SIMD and string routines, and that is what
# surfaced the ARM64 vector-add crash and the four ConstantInt::get assertions.
# It is a harder measurement than the x86-64 corpus, not the same one, so it
# does not belong in a parity gate.
#
# gcc only. There is no cross-clang in the distribution's toolchain packages,
# so the compiler dimension the x86-64 corpus has (gcc and clang) is not
# available here; that is a gap in this corpus, not a choice.
#
# Usage: bash scripts/build_multiarch_corpus.sh [--out DIR] [--opts "O0 O2"]
#                                               [--arch LIST] [--quiet]
#                                               [--link dynamic|static]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/tests/algorithm_recovery/sources"
OUT="${ROOT}/tests/algorithm_recovery/corpus-multiarch"
OPTS="O0"
QUIET=0
LINK="dynamic"

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
		--link)
			case "$2" in
				dynamic|static) LINK="$2" ;;
				*) echo "build_multiarch_corpus: --link takes dynamic or static" >&2; exit 2 ;;
			esac
			shift 2 ;;
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
	link_mode=""
	[ "${LINK}" = static ] && link_mode="-static"
	for pair in ${ARCHES}; do
		arch="${pair%%:*}"; triple="${pair##*:}"
		for opt in ${OPTS}; do
			name="${stem}-${arch}-gcc-${opt}"
			if "${triple}-gcc" "-${opt}" -std=c11 ${link_mode} \
					-fno-inline -fno-builtin \
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

# The worry that made this default to -static was that a dynamic cross binary
# would hold nothing but PLT stubs. It does not, and this says so every run
# rather than leaving it to be re-argued: `main` must be a defined text symbol
# in every binary produced. If a future toolchain change ever does hollow these
# out, the corpus fails to build instead of quietly measuring nothing.
hollow=0
unverified=0
for pair in ${ARCHES}; do
	arch="${pair%%:*}"; triple="${pair##*:}"
	if ! command -v "${triple}-nm" >/dev/null 2>&1; then
		# ${triple}-gcc is required above and comes from the same binutils
		# packaging, so this should not happen. Skipping silently would make
		# the check weaker than the thing it stands in for, which is the
		# failure mode it exists to prevent, so it fails instead.
		echo "build_multiarch_corpus: ${triple}-nm not found; cannot verify ${arch}" >&2
		unverified=$((unverified + 1))
		continue
	fi
	for bin in "${OUT}"/*-"${arch}"-gcc-*; do
		[ -f "${bin}" ] || continue
		if ! "${triple}-nm" "${bin}" 2>/dev/null | grep -qE '^[0-9a-f]+ [Tt] main$'; then
			echo "build_multiarch_corpus: no defined 'main' in ${bin}" >&2
			hollow=$((hollow + 1))
		fi
	done
done
if [ "${hollow}" != 0 ]; then
	echo "build_multiarch_corpus: ${hollow} binary/binaries carry no code of their own" >&2
	exit 1
fi
if [ "${unverified}" != 0 ]; then
	echo "build_multiarch_corpus: ${unverified} architecture(s) could not be checked" >&2
	exit 1
fi
