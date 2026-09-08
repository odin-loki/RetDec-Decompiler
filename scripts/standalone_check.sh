#!/usr/bin/env bash
#
# scripts/standalone_check.sh — build and run the LLVM-free part of RetDec with
# nothing but a C++17 compiler.
#
# Why this exists
# ---------------
# A normal `cmake --preset core-release` build downloads and compiles LLVM,
# Capstone, Keystone, YARA, YaraMod and GoogleTest.  That needs network access
# and hours of CPU, which means the ordinary answer to "does my detector change
# still pass its tests?" is "wait for CI".
#
# But most of the src/ modules — the whole Imortek detector, SSA, codegen, type
# and bytecode-parser layer — depend only on the in-house `retdec/ssa` IR, the
# C++17 standard library, and two headers vendored in deps/.  This script compiles those, links their existing
# GoogleTest suites against the shim in tests/standalone/gtest/, and runs them.
#
# Usage
#   scripts/standalone_check.sh                  # build + run every suite
#   scripts/standalone_check.sh algo_recover ssa # only these suites
#   scripts/standalone_check.sh --list           # list known modules and suites
#   scripts/standalone_check.sh --compile-only   # syntax/codegen check, no tests
#   scripts/standalone_check.sh --clean          # drop the object cache
#
# Environment
#   CXX           compiler to use               (default: g++)
#   JOBS          parallel compile jobs         (default: nproc)
#   BUILD_DIR     object cache location         (default: build/standalone)
#   EXTRA_CXXFLAGS  appended to every compile   (e.g. "-fsanitize=address -g")
#
# Exit status is non-zero if any module fails to compile or any test fails.

set -u -o pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CXX="${CXX:-g++}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
BUILD_DIR="${BUILD_DIR:-build/standalone}"
EXTRA_CXXFLAGS="${EXTRA_CXXFLAGS:-}"

# Header-only dependencies that are vendored in the tree, so they cost nothing
# to use here.  Everything else under deps/ is a download stub and stays out.
INCLUDES="-Iinclude -Ideps/rapidjson/include -Ideps/whereami"

# RAPIDJSON_HAS_STDSTRING mirrors deps/rapidjson/CMakeLists.txt; retdec/serdes
# passes std::string straight to rapidjson and does not compile without it.
# The version strings are normally stamped in by the top-level CMakeLists; a
# standalone run is not a release build, so it says so.
DEFINES="-DRAPIDJSON_HAS_STDSTRING=1"
DEFINES="$DEFINES -DRETDEC_GIT_COMMIT_HASH=\"standalone\""
DEFINES="$DEFINES -DRETDEC_GIT_VERSION_TAG=\"standalone\""
DEFINES="$DEFINES -DRETDEC_BUILD_DATE=\"standalone\""

# -Wno-unused-parameter: the upstream style leaves interface parameters named.
CXXFLAGS="-std=c++17 $INCLUDES $DEFINES -O1 -g0 -fno-omit-frame-pointer -Wall -Wno-unused-parameter ${EXTRA_CXXFLAGS}"
TESTFLAGS="-std=c++17 $INCLUDES $DEFINES -Itests/standalone -O1 -g0 -Wall -Wno-unused-parameter ${EXTRA_CXXFLAGS}"

# Modules that build with no third-party dependency.  Keep alphabetical.
# A module belongs here only if `$CXX -std=c++17 $INCLUDES $DEFINES -fsyntax-only src/<m>/*.cpp`
# succeeds; scripts/standalone_check.sh --audit re-verifies that claim.
readonly MODULES=(
	algo_recover alias_analysis bc_module call_conv cfg cfg_structure
	cil_reconstruct cli_parser code_data codegen common compiler_abi
	compiler_detect concurrency_detect config container_detect crypto_detect
	csharp_emitter ctypes ctypesparser cuda_accel cxx_backend dce debug_info
	dex_parser eh_reconstruct experimental fsharp_emitter func_boundary
	idiom_reconstruct ipa java_emitter jvm_parser jvm_reconstruct kotlin_emitter
	loader_sim lua_parser mini_emu module_cluster neural packer pattern_detect
	pdbparser pelib profiling ptx_decompile py_emitter py_reconstruct pyc_parser
	rtti serdes serial_detect sort_detect ssa string_detect testing
	type_inference type_seed utils var_recovery vbnet_emitter wasm_parser
)

# Test suites that build against the shim.  Each entry is a tests/ subdirectory
# whose sources use only the supported GoogleTest subset (no gmock, no death
# tests).  Suites outside this list still build the normal way through CMake --
# tests/utils, for one, includes <gmock/gmock.h>.
#
# `bounds` covers the header-only retdec/utils/bounds.h and leb128.h, so it has
# no entry in MODULES; it links against nothing.
readonly SUITES=(
	algo_recover alias_analysis bc_module bounds call_conv cfg cfg_structure
	cil_reconstruct cli_parser code_data codegen common compiler_abi
	compiler_detect concurrency_detect config container_detect crypto_detect
	csharp_emitter ctypes ctypesparser cuda_accel cxx_backend dce debug_info
	dex_parser eh_reconstruct fileformat fsharp_emitter func_boundary
	idiom_reconstruct
	ipa java_emitter jvm_parser jvm_reconstruct kotlin_emitter loader_sim
	lua_parser mini_emu module_cluster neural packer pattern_detect profiling
	pdbparser ptx_decompile py_emitter py_reconstruct pyc_parser retdec rtti serdes
	serial_detect sort_detect ssa string_detect testing type_inference
	type_seed utils var_recovery vbnet_emitter wasm_parser
)

# Test suites deliberately NOT run here, with the reason.  --audit consults this
# so a suite whose module is already in the fast path cannot be silently
# forgotten -- 933 test cases were, until it started checking.
readonly EXCLUDED_SUITES=(
)

# Individual test files that must not be built here, with the reason.
# "suite/file.cpp:reason"
#
# The inverse of PARTIAL_SUITES, and the difference matters: PARTIAL_SUITES
# names the files that DO build, so a test added later is silently left out,
# while this names the ones that do not and picks up everything else by default.
#
# `utils` was excluded wholesale for gmock. Two of its thirteen files include
# it; the other eleven -- alignment, conversion, string, math, container, array,
# filter_iterator, memory, scope_exit, time, binary_path -- had never run in the
# fast path because of those two. That is the module the proved kernels live in.
readonly EXCLUDED_TEST_SOURCES=(
	"utils/byte_value_storage_tests.cpp:includes <gmock/gmock.h>, which the shim does not provide"
	"utils/version_tests.cpp:includes <gmock/gmock.h>, which the shim does not provide"
)

# Modules compiled here that have no tests/ directory at all, with the reason.
#
# The SUITES drift check below can only see a suite that exists: a module with
# no test directory is invisible to it, which is how pdbparser and pelib came to
# be compiled on every run, fuzzed, and repeatedly fixed for memory safety while
# nothing asserted anything about either of them. A module belongs here only
# when having no unit tests is a decision somebody made, not an oversight
# nobody noticed.
readonly UNTESTED_MODULES=(
	"experimental:staging area for code that has not settled; nothing here is API yet"
	"pelib:vendored from the upstream PeLib fork and tracked against it, so tests live with the fuzz corpus in tests/managed_integration/fuzz/fuzz_pelib.cpp"
)

# Modules whose own CMakeLists asks for a later language standard than the rest
# of the tree.  Kept in sync by --audit, which probes each module at its
# declared standard.
readonly CXX20_MODULES=(
	cli_parser
	cuda_accel
)

# Modules with .cu sources.  Their CMakeLists compiles those with nvcc when CUDA
# is present and as plain C++ otherwise, with the device code behind #ifdef; the
# CPU path is what this script builds, so the .cu files are compiled as C++ too.
# Without this the module's objects are missing and its suite fails to link --
# which is how it was found.
readonly CU_AS_CXX_MODULES=(
	cuda_accel
)

# Individual sources from a module whose directory is otherwise excluded.  The
# module as a whole needs LLVM; these files do not.  Selecting by directory
# would leave them untested, and semantic_recovery_export.cpp in particular is
# the 1773-line emitter behind the --buildable sidecar.
# An entry may carry ":c++20" when the file needs a later standard than the
# module default, the way CXX20_MODULES does for whole modules.
readonly EXTRA_SOURCES=(
	"retdec/semantic_recovery_export.cpp"
	"retdec/neural_refine_stub.cpp"
	# The managed-format router. src/retdec-decompiler/ as a whole is the CLI
	# front end and needs LLVM, but this file needs only the bytecode modules
	# this script already builds -- and it is what decides, from the first bytes
	# of an untrusted file, which parser sees it. It was reachable by nothing
	# here until tests/retdec/managed_decompiler_test.cpp.
	"retdec-decompiler/managed_decompiler.cpp:c++20"
	# The signature-lattice parser: the first structured read of an untrusted
	# file, and the only translation unit under src/fileformat/ that does not
	# link LLVM. tests/fileformat/format_lattice_test.cpp needs nothing else
	# either, so both come into the fast gate through PARTIAL_SUITES below.
	"fileformat/lattice/format_lattice.cpp"
	# The logger. src/utils/io/ is a subdirectory, and the module glob above is
	# `src/<module>/*.cpp`, which does not recurse -- so the process-wide writer
	# table every thread of this tree logs through was compiled by nothing here,
	# and tests/utils/log_tests.cpp could not link.
	"utils/io/log.cpp"
	"utils/io/logger.cpp"
)

# Test suites where only some files build here, for the same reason.  The rest
# of the directory keeps building through CMake.
# "suite:file.cpp file.cpp"
readonly PARTIAL_SUITES=(
	"retdec:semantic_recovery_export_test.cpp thread_pool_test.cpp managed_decompiler_test.cpp"
	# The other twelve files in tests/fileformat/ drive retdec::fileformat,
	# which publicly links LLVM. This one drives the lattice, which does not.
	"fileformat:format_lattice_test.cpp"
)

# Sources inside an included module that must NOT be compiled here, mirroring a
# conditional in that module's CMakeLists.  "module/file.cpp:reason".
readonly EXCLUDED_SOURCES=(
	"neural/llama_inference.cpp:only built with retdec::deps::llamacpp; mock_inference.cpp provides createLlamaInference() otherwise"
)

# Modules that happen to compile standalone but are deliberately not part of the
# fast path, with the reason.  --audit consults this so it can distinguish
# "nobody wired this up" from "we decided not to".
readonly EXCLUDED_REASONS=(
	"demanglertool:command-line tool with its own main(), not a library"
)

C_GREEN=''; C_RED=''; C_YELLOW=''; C_DIM=''; C_OFF=''
if [ -t 1 ] && [ "${TERM:-dumb}" != dumb ]; then
	C_GREEN=$'\033[0;32m'; C_RED=$'\033[0;31m'; C_YELLOW=$'\033[0;33m'
	C_DIM=$'\033[0;90m'; C_OFF=$'\033[m'
fi

say()  { printf '%s\n' "$*"; }
ok()   { printf '%s  ok  %s%s\n' "$C_GREEN" "$C_OFF" "$*"; }
bad()  { printf '%s FAIL %s%s\n' "$C_RED" "$C_OFF" "$*"; }
skip() { printf '%s skip %s%s\n' "$C_YELLOW" "$C_OFF" "$*"; }
hdr()  { printf '\n%s== %s ==%s\n' "$C_DIM" "$*" "$C_OFF"; }

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

MODE=run
declare -a WANTED=()
while [ $# -gt 0 ]; do
	case "$1" in
		--list)         MODE=list ;;
		--compile-only) MODE=compile ;;
		--audit)        MODE=audit ;;
		--clean)        MODE=clean ;;
		-h|--help)      usage; exit 0 ;;
		-*)             say "unknown option: $1"; usage; exit 2 ;;
		*)              WANTED+=("$1") ;;
	esac
	shift
done

if [ "$MODE" = clean ]; then
	rm -rf "$BUILD_DIR"
	say "removed $BUILD_DIR"
	exit 0
fi

if [ "$MODE" = list ]; then
	say "modules (${#MODULES[@]}):"
	printf '  %s\n' "${MODULES[@]}"
	say ""
	say "suites (${#SUITES[@]}):"
	printf '  %s\n' "${SUITES[@]}"
	exit 0
fi

# `--audit` re-derives the dependency-free module list from the tree, so the
# hard-coded MODULES array cannot silently rot as modules gain dependencies or
# new dependency-free modules appear.
if [ "$MODE" = audit ]; then
	hdr "auditing which src/ modules are dependency-free"
	status=0
	declare -A declared=()
	for m in "${MODULES[@]}"; do declared["$m"]=1; done
	declare -A excluded=()
	for entry in "${EXCLUDED_REASONS[@]}"; do excluded["${entry%%:*}"]=1; done
	declare -A skipAudit=()
	for entry in "${EXCLUDED_SOURCES[@]}"; do skipAudit["${entry%%:*}"]=1; done
	for d in src/*/; do
		m="$(basename "$d")"
		shopt -s nullglob
		local_srcs=("$d"*.cpp)
		shopt -u nullglob
		srcs=()
		for f in "${local_srcs[@]}"; do
			[ -n "${skipAudit[$m/$(basename "$f")]:-}" ] && continue
			srcs+=("$f")
		done
		for c20 in "${CU_AS_CXX_MODULES[@]}"; do
			if [ "$c20" = "$m" ]; then
				shopt -s nullglob
				srcs+=("$d"*.cu)
				shopt -u nullglob
			fi
		done
		[ ${#srcs[@]} -eq 0 ] && continue
		std=c++17
		for c20 in "${CXX20_MODULES[@]}"; do [ "$c20" = "$m" ] && std=c++20; done
		# shellcheck disable=SC2086
		if $CXX -std=$std $INCLUDES $DEFINES -fsyntax-only -x c++ "${srcs[@]}" >/dev/null 2>&1; then
			if [ -z "${declared[$m]:-}" ] && [ -z "${excluded[$m]:-}" ]; then
				bad "$m compiles standalone but is missing from MODULES"
				status=1
			fi
		elif [ -n "${declared[$m]:-}" ]; then
			bad "$m is listed in MODULES but no longer compiles standalone"
			status=1
		fi
	done

	# The same drift check for SUITES: a tests/ directory whose module is
	# already compiled here, that is neither run nor explicitly excluded, is a
	# suite nobody wired up.
	declare -A declaredSuite=()
	for t in "${SUITES[@]}"; do declaredSuite["$t"]=1; done
	declare -A excludedSuite=()
	for entry in "${EXCLUDED_SUITES[@]}"; do excludedSuite["${entry%%:*}"]=1; done

	for d in tests/*/; do
		t="$(basename "$d")"
		shopt -s nullglob
		tsrcs=("$d"*.cpp)
		shopt -u nullglob
		[ ${#tsrcs[@]} -eq 0 ] && continue
		[ -n "${declaredSuite[$t]:-}" ] && continue
		[ -n "${excludedSuite[$t]:-}" ] && continue
		# Only complain when the code under test is already being compiled.
		if [ -n "${declared[$t]:-}" ]; then
			bad "$t has a test suite and its module is in the fast path, but SUITES does not run it"
			status=1
		fi
	done

	# And the case neither check above can see: a module compiled on every run
	# with no tests/ directory at all. The SUITES check only looks at suites
	# that exist, so a module nobody ever wrote a test for is invisible to it --
	# which is how a parser of untrusted input can be fuzzed and fixed for
	# memory safety a dozen times while nothing asserts anything about it.
	declare -A untested=()
	for entry in "${UNTESTED_MODULES[@]}"; do untested["${entry%%:*}"]=1; done
	for m in "${MODULES[@]}"; do
		[ -n "${untested[$m]:-}" ] && continue
		shopt -s nullglob
		tsrcs=(tests/"$m"/*.cpp)
		shopt -u nullglob
		if [ ${#tsrcs[@]} -eq 0 ]; then
			bad "$m is compiled on every run but has no tests/$m -- add a suite, or say why not in UNTESTED_MODULES"
			status=1
		fi
	done

	[ $status -eq 0 ] && ok "MODULES and SUITES match the tree"
	exit $status
fi

mkdir -p "$BUILD_DIR/obj" "$BUILD_DIR/lib" "$BUILD_DIR/bin"

# The object cache is keyed on modification time, which cannot see a flag
# change.  So one run with EXTRA_CXXFLAGS="-fsanitize=address" leaves sanitized
# objects in the build directory, and the next plain run happily reuses them and
# dies at the link with undefined __asan_report_load8 -- an error that says
# nothing about what actually happened, in a directory the user did not think
# they had touched.  Stamp what the objects were built with, and start over when
# it changes.
BUILD_STAMP="$BUILD_DIR/.build-flags"
BUILD_KEY="$CXX|${CC:-cc}|$CXXFLAGS|$TESTFLAGS"
if [ -f "$BUILD_STAMP" ] && [ "$(cat "$BUILD_STAMP")" != "$BUILD_KEY" ]; then
	hdr "compiler or flags changed since the last build in $BUILD_DIR — rebuilding"
	rm -rf "$BUILD_DIR/obj" "$BUILD_DIR/lib" "$BUILD_DIR/bin"
	mkdir -p "$BUILD_DIR/obj" "$BUILD_DIR/lib" "$BUILD_DIR/bin"
fi
printf '%s\n' "$BUILD_KEY" > "$BUILD_STAMP"

# ── compile one source file if its object is stale ───────────────────────────
# Takes a single tab-separated "source<TAB>object<TAB>kind" record.  Flags are
# passed through the environment rather than the record because they contain
# spaces, and xargs word-splits on blanks.
compile_one() {
	local line="$1"
	local src obj kind flags
	src="${line%%$'\t'*}"
	line="${line#*$'\t'}"
	obj="${line%%$'\t'*}"
	kind="${line#*$'\t'}"

	local compiler="$CXX"
	case "$kind" in
		test)  flags="$SC_TESTFLAGS" ;;
		test20) flags="${SC_TESTFLAGS/-std=c++17/-std=c++20}" ;;
		# whereami is vendored C, not C++; retdec/utils links against it.
		cc)    flags="-O1 -g0 -w $SC_EXTRA_CXXFLAGS"; compiler="${CC:-cc}" ;;
		mod20)   flags="${SC_CXXFLAGS/-std=c++17/-std=c++20}" ;;
		# -x c++ so the compiler does not refuse an unknown .cu extension.
		modcu)   flags="$SC_CXXFLAGS -x c++" ;;
		mod20cu) flags="${SC_CXXFLAGS/-std=c++17/-std=c++20} -x c++" ;;
		*)     flags="$SC_CXXFLAGS" ;;
	esac

	# Is the cached object still good?
	#
	# This used to be "the object is newer than the .cpp and than this script",
	# which does not mention headers at all -- so ANY header change produced a
	# green run against stale objects. That is not a hypothetical: removing one
	# `#include <cassert>` from include/retdec/utils/container.h left this check
	# reporting "all 62 dependency-free modules compile" while a build from an
	# empty cache failed on src/ctypes/enum_type.cpp. A fast check that can
	# report green on a tree which does not build is worse than no check, since
	# it is trusted.
	#
	# -MMD writes the header list the compiler actually used to $obj.d, and each
	# one is compared against the object below. No dependency file means no
	# knowledge, so the object is rebuilt rather than assumed good.
	if [ -f "$obj" ] && [ -f "$obj.d" ] \
			&& [ "$obj" -nt "$src" ] && [ "$obj" -nt "$SC_SELF" ]; then
		local deps stale=0
		# Drop the "target:" prefix and the line continuations; what is left is
		# the prerequisite list. Unquoted on purpose so it word-splits -- no
		# path in this tree contains a space, and -MMD would have escaped one.
		deps="$(sed -e '1s/^[^:]*://' -e 's/\\$//' "$obj.d" 2>/dev/null)"
		# shellcheck disable=SC2086
		for h in $deps; do
			[ -e "$h" ] || { stale=1; break; }
			[ "$obj" -nt "$h" ] || { stale=1; break; }
		done
		[ $stale -eq 0 ] && return 0
	fi
	mkdir -p "$(dirname "$obj")"
	# shellcheck disable=SC2086
	if ! $compiler $flags -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.log"; then
		# A failed compile can leave a partial dependency file, which would then
		# look like knowledge on the next run.
		rm -f "$obj.d"
		return 1
	fi
	rm -f "$obj.log"
	return 0
}
export -f compile_one
export CXX
export SC_CXXFLAGS="$CXXFLAGS"
export SC_TESTFLAGS="$TESTFLAGS"
export SC_SELF="$ROOT/scripts/standalone_check.sh"
export SC_EXTRA_CXXFLAGS="$EXTRA_CXXFLAGS"
export CC

# Build a job list, then run it with xargs -P for parallelism.
JOBLIST="$BUILD_DIR/jobs.txt"
: > "$JOBLIST"

selected_modules=("${MODULES[@]}")
if [ ${#WANTED[@]} -gt 0 ]; then
	# When specific suites are named, still build every module: link-time
	# dependencies between modules are not declared anywhere machine-readable,
	# and a full object cache costs one build.
	:
fi

declare -A skip_source=()
for entry in "${EXCLUDED_SOURCES[@]}"; do skip_source["${entry%%:*}"]=1; done
declare -A is_cxx20=()
for m in "${CXX20_MODULES[@]}"; do is_cxx20["$m"]=1; done

declare -A has_cu=()
for m in "${CU_AS_CXX_MODULES[@]}"; do has_cu["$m"]=1; done

for m in "${selected_modules[@]}"; do
	kind=mod
	[ -n "${is_cxx20[$m]:-}" ] && kind=mod20
	shopt -s nullglob
	for src in "src/$m"/*.cpp; do
		[ -n "${skip_source[$m/$(basename "$src")]:-}" ] && continue
		printf '%s\t%s\t%s\n' "$src" "$BUILD_DIR/obj/$m/$(basename "${src%.cpp}").o" "$kind" >> "$JOBLIST"
	done
	if [ -n "${has_cu[$m]:-}" ]; then
		for src in "src/$m"/*.cu; do
			printf '%s\t%s\t%s\n' "$src" "$BUILD_DIR/obj/$m/$(basename "${src%.cu}").o" "${kind}cu" >> "$JOBLIST"
		done
	fi
	shopt -u nullglob
done
for extraSpec in "${EXTRA_SOURCES[@]}"; do
	extra="${extraSpec%%:*}"
	extraStd="${extraSpec#*:}"
	[ "$extraStd" = "$extraSpec" ] && extraStd=c++17
	[ -f "src/$extra" ] || continue
	extraKind=mod
	[ "$extraStd" = c++20 ] && extraKind=mod20
	printf '%s\t%s\t%s\n' "src/$extra" \
		"$BUILD_DIR/obj/$(dirname "$extra")/$(basename "${extra%.cpp}").o" \
		"$extraKind" >> "$JOBLIST"
done
printf '%s\t%s\tcc\n' deps/whereami/whereami/whereami.c "$BUILD_DIR/obj/utils/whereami.o" >> "$JOBLIST"
printf '%s\t%s\ttest\n' tests/standalone/gtest_lite.cpp "$BUILD_DIR/obj/gtest_lite.o" >> "$JOBLIST"
printf '%s\t%s\ttest\n' tests/standalone/gtest_lite_main.cpp "$BUILD_DIR/obj/gtest_lite_main.o" >> "$JOBLIST"

hdr "compiling $(wc -l < "$JOBLIST") translation units with $CXX (-j$JOBS)"
compile_status=0
if ! xargs -d '\n' -P "$JOBS" -n 1 bash -c 'compile_one "$0"' < "$JOBLIST"; then
	compile_status=1
fi

if [ $compile_status -ne 0 ]; then
	bad "compilation failed:"
	find "$BUILD_DIR/obj" -name '*.log' -size +0 | while read -r log; do
		printf '\n%s--- %s%s\n' "$C_RED" "${log%.o.log}" "$C_OFF"
		head -30 "$log"
	done
	exit 1
fi
ok "all ${#MODULES[@]} dependency-free modules compile"

# ── the CUDA half, which the build above does not reach ─────────────────────
#
# src/utils/CMakeLists.txt builds retdec-gpu-scanner from gpu_scanner.cu when
# CUDA is present and from gpu_scanner_cpu.cpp otherwise. Only the second is
# ever built here, and it includes the first with RETDEC_GPU_SCANNER_HOST_ONLY
# defined -- so roughly 660 lines, all of the device code and all of the CUDA
# plumbing, were compiled by NOTHING. An adversarial verify pass proved the
# consequence by reverting four fixes inside that region and watching the suite
# stay green.
#
# tests/utils/cuda_stub/ is enough of the CUDA runtime to compile it with an
# ordinary C++ compiler. This is a syntax check, not a link: the class's methods
# are already defined by gpu_scanner_cpu.o, so the two cannot go in one binary.
# What it catches is everything a compiler catches -- and it caught a real one
# the first time it ran, an unqualified `gpuscan::` that does not resolve from
# the device code's namespace, introduced by a change to a file nobody compiles.
compile_cuda_half() {
	local src="src/utils/gpu_scanner.cu"
	[ -f "$src" ] || return 0
	local tu; tu="$(mktemp --suffix=.cpp)"
	# No RETDEC_GPU_SCANNER_HOST_ONLY: that is the point.
	{
		printf '#include "cuda_runtime.h"\n'
		printf '#include "retdec/utils/gpu_scanner.h"\n'
		printf '#include "%s"\n' "$ROOT/$src"
	} > "$tu"
	local log; log="$(mktemp)"
	if "$CXX" $CXXFLAGS -Wall -Wextra -fsyntax-only \
			-I"$ROOT/tests/utils/cuda_stub" -x c++ "$tu" > "$log" 2>&1; then
		ok "gpu_scanner.cu compiles against the CUDA stubs"
		rm -f "$tu" "$log"
		return 0
	fi
	bad "gpu_scanner.cu does not compile against tests/utils/cuda_stub:"
	head -30 "$log"
	rm -f "$tu" "$log"
	return 1
}
compile_cuda_half || exit 1

# ── the proved kernels, standalone, under the project's own warning flags ────
#
# include/retdec/utils/ is the set retdec/utils/bounds.h and its siblings live
# in: header-only, proved by tests/verification/, and included from all over the
# tree. Two things can go wrong there that nothing else here would notice.
#
# A header that does not compile on its own compiles anyway wherever some
# earlier include happened to declare what it uses. ord_lookup.h named
# std::string and std::size_t with neither <string> nor <cstddef>; scope_exit.h
# used std::forward with no <utility>.
#
# And the project compiles with -Wswitch-default and -Wreorder (CMakeLists.txt),
# while src/neural adds -Werror. RETDEC_ENABLE_NEURAL is ON by default and
# src/neural/gates.cpp includes c_source_scan.h, whose switch had no default --
# so a default build of this repository did not compile, and nothing in the fast
# gate could see it, because the fast gate does not use those flags.
#
# One translation unit per header, which is the only way to ask the first
# question, with the project's flags, which is the only way to ask the second.
KERNEL_WARN_FLAGS="-Wall -Wextra -Wswitch-default -Werror"
check_kernel_headers() {
	shopt -s nullglob
	local headers=(include/retdec/utils/*.h)
	shopt -u nullglob
	[ ${#headers[@]} -eq 0 ] && return 0

	local tu log h bad_headers=()
	tu="$(mktemp --suffix=.cpp)"
	log="$(mktemp)"
	for h in "${headers[@]}"; do
		printf '#include "%s"\nint main(void) { return 0; }\n' "${h#include/}" > "$tu"
		# shellcheck disable=SC2086
		if ! $CXX -std=c++17 $INCLUDES $DEFINES $KERNEL_WARN_FLAGS $EXTRA_CXXFLAGS \
				-fsyntax-only "$tu" > "$log" 2>&1; then
			bad_headers+=("$h")
			printf '\n%s--- %s%s\n' "$C_RED" "$h" "$C_OFF"
			head -12 "$log"
		fi
	done
	rm -f "$tu" "$log"

	if [ ${#bad_headers[@]} -gt 0 ]; then
		bad "not standalone, or not warning-clean at ${KERNEL_WARN_FLAGS}: ${bad_headers[*]}"
		return 1
	fi
	ok "${#headers[@]} kernel headers compile standalone, warnings as errors"
	return 0
}
check_kernel_headers || exit 1

# ── archive each module so the linker pulls only what a suite needs ──────────
for m in "${selected_modules[@]}"; do
	shopt -s nullglob
	objs=("$BUILD_DIR/obj/$m"/*.o)
	shopt -u nullglob
	[ ${#objs[@]} -eq 0 ] && continue
	# Recreate rather than update: `ar r` keeps members whose source has since
	# been deleted, renamed or excluded, which shows up as a duplicate-symbol
	# link error long after the fact.
	rm -f "$BUILD_DIR/lib/lib$m.a"
	ar qcs "$BUILD_DIR/lib/lib$m.a" "${objs[@]}"
done
for extraSpec in "${EXTRA_SOURCES[@]}"; do
	extra="${extraSpec%%:*}"
	# The object still lives under its real directory; only the archive name is
	# flattened, because libextra_fileformat/lattice.a would be a path rather
	# than a name -- ar would need the directory to exist and the libargs glob
	# below would not match it.
	objDir="$(dirname "$extra")"
	d="$(printf '%s' "$objDir" | tr '/' '_')"
	shopt -s nullglob
	eobjs=("$BUILD_DIR/obj/$objDir"/*.o)
	shopt -u nullglob
	# Recreate, like the module archives above. This used to be guarded by
	# `[ ! -f ... ]`, so once the archive existed it was never rebuilt: an
	# EXTRA_SOURCES file could be edited, recompiled and still linked from the
	# object the archive was made from. Every load-bearing check against one of
	# these files reported the fix as doing nothing, which is the worst
	# direction for that error to point.
	if [ ${#eobjs[@]} -gt 0 ]; then
		rm -f "$BUILD_DIR/lib/libextra_$d.a"
		ar qcs "$BUILD_DIR/lib/libextra_$d.a" "${eobjs[@]}"
	fi
done
rm -f "$BUILD_DIR/lib/libgtest_lite_main.a"
ar qcs "$BUILD_DIR/lib/libgtest_lite_main.a" "$BUILD_DIR/obj/gtest_lite_main.o"

if [ "$MODE" = compile ]; then
	ok "compile-only run finished"
	exit 0
fi

# ── build and run the suites ────────────────────────────────────────────────
run_suites=("${SUITES[@]}")
if [ ${#WANTED[@]} -gt 0 ]; then
	run_suites=()
	for w in "${WANTED[@]}"; do
		found=0
		for s in "${SUITES[@]}"; do [ "$s" = "$w" ] && found=1; done
		if [ $found -eq 1 ]; then
			run_suites+=("$w")
		else
			skip "$w is not a standalone suite (see --list)"
		fi
	done
fi

hdr "building and running ${#run_suites[@]} test suite(s)"

declare -a libargs=()
for m in "${selected_modules[@]}"; do
	[ -f "$BUILD_DIR/lib/lib$m.a" ] && libargs+=("$BUILD_DIR/lib/lib$m.a")
done
shopt -s nullglob
for extralib in "$BUILD_DIR/lib"/libextra_*.a; do libargs+=("$extralib"); done
shopt -u nullglob

failed_suites=()
passed=0
for s in "${run_suites[@]}"; do
	srcs=()
	partial=""
	for entry in "${PARTIAL_SUITES[@]}"; do
		[ "${entry%%:*}" = "$s" ] && partial="${entry#*:}"
	done
	if [ -n "$partial" ]; then
		for f in $partial; do
			[ -f "tests/$s/$f" ] && srcs+=("tests/$s/$f")
		done
	else
		shopt -s nullglob
		local_tsrcs=(tests/"$s"/*.cpp)
		shopt -u nullglob
		srcs=()
		for f in "${local_tsrcs[@]}"; do
			skipThis=""
			for entry in "${EXCLUDED_TEST_SOURCES[@]}"; do
				[ "${entry%%:*}" = "$s/$(basename "$f")" ] && skipThis=1
			done
			[ -n "$skipThis" ] && continue
			srcs+=("$f")
		done
	fi
	if [ ${#srcs[@]} -eq 0 ]; then
		skip "$s (no sources)"
		continue
	fi

	bin="$BUILD_DIR/bin/$s"
	# --start-group/--end-group resolves the undeclared cycles between module
	# archives without needing a topological order.
	suiteFlags="$TESTFLAGS"
	for c20 in "${CXX20_MODULES[@]}"; do [ "$c20" = "$s" ] && suiteFlags="${TESTFLAGS/-std=c++17/-std=c++20}"; done

	# The archive readers (JAR, APK) call into zlib, which is a system library
	# rather than anything vendored; link it when it is present.
	extraLibs=""
	if [ -n "${ZLIB_LIB:-}" ] || printf 'int main(void){return 0;}' | $CXX -x c++ - -lz -o /dev/null >/dev/null 2>&1; then
		extraLibs="${ZLIB_LIB:--lz}"
	fi
	# shellcheck disable=SC2086
	if ! $CXX $suiteFlags "${srcs[@]}" \
			-Wl,--start-group "${libargs[@]}" "$BUILD_DIR/lib/libgtest_lite_main.a" -Wl,--end-group \
			"$BUILD_DIR/obj/gtest_lite.o" $extraLibs \
			-o "$bin" > "$bin.buildlog" 2>&1; then
		bad "$s (build)"
		# The errors, then the head. `head -25` on its own put the first
		# twenty-five lines on screen, and in a suite whose headers emit
		# warnings those are twenty-five warnings and the error is not among
		# them -- which is exactly what happened while adding a test to
		# tests/utils: "suites passed: 0 / 1" with nothing on screen to say
		# why. Both, because a link error has no "error:" in it.
		grep -nE 'error|undefined reference|cannot find' "$bin.buildlog" | head -15
		say "  --- first lines of $bin.buildlog ---"
		head -15 "$bin.buildlog"
		failed_suites+=("$s")
		continue
	fi
	rm -f "$bin.buildlog"

	if out="$("$bin" --gtest_brief 2>&1)"; then
		ok "$s — $(printf '%s' "$out" | grep -o '[0-9]* tests\? ran' | head -1)"
		passed=$((passed + 1))
	else
		bad "$s (tests)"
		printf '%s\n' "$out" | tail -40
		failed_suites+=("$s")
	fi
done

hdr "summary"
say "suites passed: $passed / ${#run_suites[@]}"
if [ ${#failed_suites[@]} -gt 0 ]; then
	bad "failing: ${failed_suites[*]}"
	exit 1
fi
ok "standalone check clean"
