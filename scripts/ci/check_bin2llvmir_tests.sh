#!/usr/bin/env bash
# B2L-01 -- run tests/bin2llvmir against the distribution LLVM.
#
# Why this exists
# ---------------
# tests/bin2llvmir is 424 assertions over the layer that rewrites the lifted
# IR, and not one of them ran anywhere outside the pinned-LLVM build. OPT-01's
# own header said so -- "covered by shape in tests/bin2llvmir/, which this
# container cannot execute as a whole" -- and that was true of every check
# added to this repository for bin2llvmir: OPT-01 and IDIOM-USE-01 test the
# passes they name by calling them directly out of a shared object, because
# linking the real test binary was understood to need most of the tree.
#
# It does need most of the tree. What it does not need is most of the tree
# LINKED IN. A strict link of two suites with their passes and providers leaves
# 616 undefined references; the same link against an ARCHIVE, where the linker
# takes only the members something references, leaves 27, and those 27 are
# vendored sources and -lcrypto. That is the whole trick.
#
# What does not compile here, and why that is stated rather than swallowed
# -----------------------------------------------------------------------
# 23 of the tree's 952 translation units do not compile against the
# distribution LLVM 20, Capstone and gtest in this container. None of them is
# reachable from tests/bin2llvmir -- the archive link proves it, because a
# missing member that something referenced would be an undefined reference.
#
# They are listed in EXPECTED_UNCOMPILABLE below with a reason each. A
# translation unit that stops compiling and is NOT on that list fails this
# check. Quietly tolerating compile failures is how a harness comes to report
# on half of what you think it covers.
#
# Two further compromises, both stated in the same spirit:
#
#   * src/debugformat/dwarf.cpp uses the LLVM 21 DataExtractor constructor and
#     the post-20 DWARF/LowLevel/ header layout. scripts/ci/b2l_dwarf_stub.cpp
#     supplies an empty DebugFormat::loadDwarf so the link closes. No
#     bin2llvmir unit test loads DWARF; one that did would silently see no
#     debug info.
#   * tests/bin2llvmir/utils/simplifycfg_tests.cpp does
#     `#include "../lib/Transforms/Scalar/SimplifyCFGPass.cpp"`, which needs
#     the LLVM source tree and not just its headers. It is skipped, and it is
#     the only test file that is.
#
# The object cache
# ----------------
# The first run compiles 952 translation units and takes minutes. Objects are
# cached in the work directory and reused when newer than their source, so a
# re-run after touching one pass costs one compile and a link. Pass a stable
# --workdir (or B2L_WORKDIR) to keep the cache between runs.
#
# Usage: bash scripts/ci/check_bin2llvmir_tests.sh [--llvm-config PATH]
#                                                  [--workdir DIR] [--jobs N]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

LLVM_CONFIG=""
WORKDIR="${B2L_WORKDIR:-}"
JOBS="$(nproc 2>/dev/null || echo 2)"
KEEP=0

while [ $# -gt 0 ]; do
	case "$1" in
		--llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
		--workdir) WORKDIR="$2"; shift 2 ;;
		--jobs) JOBS="$2"; shift 2 ;;
		--keep) KEEP=1; shift ;;
		--self-test) shift ;;   # the expected-failure list is always checked
		*) echo "B2L-01: unknown argument $1" >&2; exit 2 ;;
	esac
done

die() { echo "B2L-01: FAIL $*" >&2; exit 1; }

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
[ -d /usr/include/gtest ] || die "gtest is not installed"

if [ -z "${WORKDIR}" ]; then
	WORKDIR="$(mktemp -d)"
	[ "${KEEP}" = 1 ] || trap 'rm -rf "${WORKDIR}"' EXIT
fi
mkdir -p "${WORKDIR}/obj" "${WORKDIR}/tobj" "${WORKDIR}/dep"

# 952 objects and the archive built from them come to something like 1.5 GB.
# Running out part-way through does not announce itself as a disk problem: ar
# reports `error reading <object>: No space left on device`, which reads like a
# corrupt object file, and a partially written archive then fails whatever
# links against it with a wall of undefined references. Ask first and say so.
NEED_KB=$((3 * 1024 * 1024))
AVAIL_KB="$(df -Pk "${WORKDIR}" | awk 'NR==2 {print $4}')"
if [ -n "${AVAIL_KB}" ] && [ "${AVAIL_KB}" -lt "${NEED_KB}" ]; then
	die "only $((AVAIL_KB / 1024)) MiB free on the filesystem holding ${WORKDIR}; this needs about 1.5 GiB for the objects and the archive, and fails confusingly when it runs out part-way"
fi

# Translation units that do not compile in this container.  A reason each, and
# none of them reachable from tests/bin2llvmir.
EXPECTED_UNCOMPILABLE="
src/bin2llvmir/optimizations/types_propagator/types_propagator.cpp|dead: resolveTypes() declared in no header; recorded in check_cmake_sources.sh UNBUILT_SRC
src/rtti-finder/vtable/vtable_xref.cpp|dead: written against an API rtti-finder no longer has; recorded in check_cmake_sources.sh UNBUILT_SRC
src/capstone2llvmir/arm/arm_init.cpp|needs the vendored Capstone; the distribution one has no ARM_INS_FCONSTD
src/capstone2llvmir/arm64/arm64.cpp|needs the vendored Capstone; no ARM64_VAS_4B
src/capstone2llvmir/arm64/arm64_init.cpp|needs the vendored Capstone; no ARM64_SYSREG_OSDTRRX_EL1
src/capstone2llvmir/powerpc/powerpc.cpp|needs the vendored Capstone; no PPC_REG_CR0LT
src/capstone2llvmir/powerpc/powerpc_init.cpp|needs the vendored Capstone; no PPC_REG_CR0LT
src/capstone2llvmir/x86/x86.cpp|needs the vendored Capstone; no X86_REG_BND0
src/capstone2llvmir/x86/x86_avx512.cpp|needs the vendored Capstone; no X86_INS_KADDB
src/capstone2llvmir/x86/x86_init.cpp|needs the vendored Capstone; no X86_REG_BND0
src/cli_parser/cil_lifter.cpp|C++20 (std::span); this harness compiles the tree as C++17
src/cli_parser/cli_heaps.cpp|C++20 (std::span)
src/cli_parser/cli_reader.cpp|C++20 (std::span)
src/cli_parser/cli_sig.cpp|C++20 (std::span)
src/cli_parser/cli_tables.cpp|C++20 (std::span)
src/cli_parser/pe_reader.cpp|C++20 (std::span)
src/opencl/ocl_buffer_pool.cpp|C++23 (std::countr_zero)
src/debugformat/dwarf.cpp|LLVM 21 DataExtractor and DWARF/LowLevel/ layout; stubbed, see the header
src/patterngen/pattern_extractor/pattern_extractor.cpp|needs yaramod built from deps/
src/patterngen/pattern_extractor/types/symbol_pattern.cpp|needs yaramod built from deps/
src/yaracpp/yara_detector.cpp|needs YARA built from deps/
src/utils/binary_path.cpp|needs deps/whereami built as C
src/utils/version.cpp|needs the RETDEC_GIT_* defines only the CMake build sets
"

LLVM_FLAGS="$("${LLVM_CONFIG}" --cxxflags | tr ' ' '\n' | grep -E '^-I|^-D' | tr '\n' ' ')"
INC="-Iinclude -Isrc -Itests -Ideps/rapidjson/include -Ideps/elfio/include \
	-Ideps/tlsh/include -Ideps/tinyxml2/include -Ideps/stb/include \
	-Ideps/authenticode-parser/include -Ideps/eigen -DRAPIDJSON_HAS_STDSTRING=1"

# Tools carry their own main() and would collide with gtest_main.
SKIP_RE='^src/(ar-extractortool|bin2pat|capstone2llvmirtool|demanglertool|fileinfo|getsig|gui|idr2pat|macho-extractortool|pat2yara|retdec-decompiler|retdectool|stacofintool|unpackertool)/'

mapfile -t SRCS < <(find src -name '*.cpp' | grep -Ev "${SKIP_RE}" | sort)
[ "${#SRCS[@]}" -ge 800 ] \
	|| die "only ${#SRCS[@]} sources found; the tree does not look complete"

echo "B2L-01: compiling ${#SRCS[@]} translation units with ${JOBS} job(s)"

compile_src() {
	f="$1"
	o="${B2L_OBJ}/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')"
	if [ -f "$o" ] && [ "$o" -nt "$f" ]; then return 0; fi
	if g++ -std=c++17 -O0 -w ${B2L_INC} ${B2L_LF} -c "$f" -o "$o" 2>"${o}.err"; then
		rm -f "${o}.err"
	else
		rm -f "$o"
	fi
}
export -f compile_src
export B2L_OBJ="${WORKDIR}/obj" B2L_INC="${INC}" B2L_LF="${LLVM_FLAGS}"

printf '%s\n' "${SRCS[@]}" | xargs -P "${JOBS}" -I{} bash -c 'compile_src "$@"' _ {}

# Every failure must be one we said would fail.
unexpected=""
for e in "${WORKDIR}"/obj/*.o.err; do
	[ -e "$e" ] || continue
	base="$(basename "$e" .o.err)"
	found=""
	while IFS='|' read -r path _reason; do
		[ -n "${path}" ] || continue
		key="$(echo "${path}" | tr '/' '_' | sed 's/\.cpp$//')"
		[ "${key}" = "${base}" ] && found=1
	done <<< "${EXPECTED_UNCOMPILABLE}"
	if [ -z "${found}" ]; then
		unexpected="${unexpected} ${base}"
		echo "--- ${base} ---" >&2
		grep -m 3 -E 'error:' "$e" >&2 || true
	fi
done
if [ -n "${unexpected}" ]; then
	die "translation unit(s) stopped compiling and are not in EXPECTED_UNCOMPILABLE:${unexpected}"
fi

# The other direction: an entry that now compiles is a stale excuse.
stale=""
while IFS='|' read -r path _reason; do
	[ -n "${path}" ] || continue
	key="$(echo "${path}" | tr '/' '_' | sed 's/\.cpp$//')"
	if [ -f "${WORKDIR}/obj/${key}.o" ]; then
		stale="${stale} ${path}"
	fi
done <<< "${EXPECTED_UNCOMPILABLE}"
if [ -n "${stale}" ]; then
	die "EXPECTED_UNCOMPILABLE names source(s) that now compile; remove them:${stale}"
fi

# Vendored sources the archive needs: tlsh, authenticode-parser and stb, the
# same three the fileformat check builds.
for f in $(find deps/tlsh -name '*.cpp' ! -name 'WinFunctions.cpp' | sort); do
	o="${WORKDIR}/dep/tlsh_$(basename "$f" .cpp).o"
	[ -f "$o" ] && [ "$o" -nt "$f" ] && continue
	g++ -std=c++17 -O0 -w -Ideps/tlsh/include -c "$f" -o "$o" \
		|| die "deps/tlsh: $f did not compile"
done
for f in $(find deps/authenticode-parser/src -name '*.c' | sort); do
	o="${WORKDIR}/dep/ac_$(basename "$f" .c).o"
	[ -f "$o" ] && [ "$o" -nt "$f" ] && continue
	gcc -O0 -w -Ideps/authenticode-parser/include -Ideps/authenticode-parser/src \
		-c "$f" -o "$o" || die "deps/authenticode-parser: $f did not compile"
done
if [ ! -f "${WORKDIR}/dep/stb_image.o" ] \
		|| [ deps/stb/stb_image.c -nt "${WORKDIR}/dep/stb_image.o" ]; then
	gcc -O0 -w -Ideps/stb/include -c deps/stb/stb_image.c \
		-o "${WORKDIR}/dep/stb_image.o" || die "deps/stb did not compile"
fi

g++ -std=c++17 -O0 -w ${INC} ${LLVM_FLAGS} -c scripts/ci/b2l_dwarf_stub.cpp \
	-o "${WORKDIR}/dwarf_stub.o" || die "the DWARF stub did not compile"

# mock_inference and neural_refine_stub are the build's stand-ins for
# llama_inference and decompile_hook; taking both of each pair is a duplicate
# definition, so the real ones win.
mapfile -t ARCHIVE_OBJS < <(ls "${WORKDIR}"/obj/*.o \
	| grep -v 'src_neural_mock_inference\.o$' \
	| grep -v 'src_retdec_neural_refine_stub\.o$')
rm -f "${WORKDIR}/libretdec.a"
if ! ar rcs "${WORKDIR}/libretdec.a" "${ARCHIVE_OBJS[@]}" 2>"${WORKDIR}/ar.err"; then
	sed -n '1,5p' "${WORKDIR}/ar.err" >&2
	if grep -q 'No space left on device' "${WORKDIR}/ar.err"; then
		die "ran out of disk building the archive; free space under ${WORKDIR} and re-run"
	fi
	die "could not build the archive"
fi

# A truncated archive links as badly as a missing one and says less about why,
# so the member count is checked rather than assumed.
MEMBERS="$(ar t "${WORKDIR}/libretdec.a" | wc -l)"
[ "${MEMBERS}" -eq "${#ARCHIVE_OBJS[@]}" ] \
	|| die "the archive holds ${MEMBERS} members and ${#ARCHIVE_OBJS[@]} were given to it; it is truncated, most likely by a full disk"

mapfile -t TEST_SRCS < <(find tests/bin2llvmir -name '*.cpp' \
	! -name 'simplifycfg_tests.cpp' | sort)
[ "${#TEST_SRCS[@]}" -ge 30 ] \
	|| die "only ${#TEST_SRCS[@]} test sources found under tests/bin2llvmir"

compile_test() {
	f="$1"
	o="${B2L_TOBJ}/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')"
	if [ -f "$o" ] && [ "$o" -nt "$f" ]; then return 0; fi
	if g++ -std=c++17 -O0 -w ${B2L_INC} ${B2L_LF} -c "$f" -o "$o" 2>"${o}.err"; then
		rm -f "${o}.err"
	else
		rm -f "$o"
	fi
}
export -f compile_test
export B2L_TOBJ="${WORKDIR}/tobj"
printf '%s\n' "${TEST_SRCS[@]}" | xargs -P "${JOBS}" -I{} bash -c 'compile_test "$@"' _ {}

for e in "${WORKDIR}"/tobj/*.o.err; do
	[ -e "$e" ] || continue
	echo "--- $(basename "$e" .o.err) ---" >&2
	grep -m 3 -E 'error:' "$e" >&2 || true
	die "a tests/bin2llvmir source did not compile"
done

g++ -std=c++17 -O0 -w "${WORKDIR}"/tobj/*.o "${WORKDIR}/dwarf_stub.o" \
	-o "${WORKDIR}/b2ltest" \
	-Wl,--start-group "${WORKDIR}/libretdec.a" "${WORKDIR}"/dep/*.o -Wl,--end-group \
	-lgtest -lgtest_main -lgmock \
	$("${LLVM_CONFIG}" --libs) $("${LLVM_CONFIG}" --system-libs) \
	-lcapstone -lz -lcrypto -pthread \
	2>"${WORKDIR}/link.err" \
	|| { grep -m 20 -E 'undefined reference|error' "${WORKDIR}/link.err" >&2; \
	     die "tests/bin2llvmir did not link"; }

if ! "${WORKDIR}/b2ltest" > "${WORKDIR}/run.log" 2>&1; then
	echo "B2L-01: FAIL tests/bin2llvmir does not pass" >&2
	echo "--- failing tests ---" >&2
	grep -aoE '^\[  FAILED  \] [A-Za-z][A-Za-z0-9_]*\.[A-Za-z0-9_]*' \
		"${WORKDIR}/run.log" | sort -u >&2
	echo "--- first assertions ---" >&2
	grep -aE 'Failure$' -A 3 "${WORKDIR}/run.log" | head -n 60 >&2
	echo "B2L-01: full log: ${WORKDIR}/run.log" >&2
	exit 1
fi

# "0 tests ran" passes every assertion it has, so the count is checked too.
RAN="$(grep -aoE '[0-9]+ tests? from [0-9]+ test suites? ran' "${WORKDIR}/run.log" \
	| tail -n1 || true)"
[ -n "${RAN}" ] || { tail -n 20 "${WORKDIR}/run.log" >&2; \
	die "could not tell how many tests ran"; }
COUNT="$(echo "${RAN}" | grep -oE '^[0-9]+')"
[ "${COUNT}" -ge 400 ] \
	|| die "only ${COUNT} tests ran; tests/bin2llvmir had 424 when this was written"

echo "B2L-01: OK ${RAN} against LLVM ${LLVM_MAJOR}"
