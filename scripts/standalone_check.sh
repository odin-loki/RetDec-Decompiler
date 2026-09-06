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
# But 56 of the src/ modules — the whole Imortek detector, SSA, codegen, type
# and bytecode-parser layer — depend only on the in-house `retdec/ssa` IR and
# the C++17 standard library.  This script compiles those, links their existing
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

# -Wno-unused-parameter: the upstream style leaves interface parameters named.
CXXFLAGS="-std=c++17 -Iinclude -O1 -g0 -fno-omit-frame-pointer -Wall -Wno-unused-parameter ${EXTRA_CXXFLAGS}"
TESTFLAGS="-std=c++17 -Iinclude -Itests/standalone -O1 -g0 -Wall -Wno-unused-parameter ${EXTRA_CXXFLAGS}"

# Modules that build with no third-party dependency.  Keep alphabetical.
# A module belongs here only if `$CXX -std=c++17 -fsyntax-only -Iinclude src/<m>/*.cpp`
# succeeds; scripts/standalone_check.sh --audit re-verifies that claim.
readonly MODULES=(
	algo_recover alias_analysis bc_module call_conv cfg cfg_structure
	cil_reconstruct code_data codegen common compiler_abi compiler_detect
	concurrency_detect container_detect crypto_detect csharp_emitter ctypes
	cuda_accel cxx_backend dce debug_info dex_parser eh_reconstruct experimental
	fsharp_emitter func_boundary idiom_reconstruct ipa java_emitter jvm_parser
	jvm_reconstruct kotlin_emitter loader_sim lua_parser mini_emu module_cluster
	packer pattern_detect pdbparser pelib profiling ptx_decompile py_emitter
	py_reconstruct pyc_parser rtti serial_detect sort_detect ssa string_detect
	testing type_inference type_seed var_recovery vbnet_emitter wasm_parser
)

# Test suites that build against the shim.  Each entry is a tests/ subdirectory
# whose sources use only the supported GoogleTest subset (no gmock, no death
# tests).  Suites outside this list still build the normal way through CMake.
readonly SUITES=(
	algo_recover alias_analysis bc_module call_conv cfg cfg_structure
	code_data codegen compiler_abi concurrency_detect container_detect
	crypto_detect dce eh_reconstruct func_boundary idiom_reconstruct ipa
	loader_sim mini_emu module_cluster pattern_detect profiling rtti
	serial_detect sort_detect ssa string_detect type_inference type_seed
	var_recovery
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
	for d in src/*/; do
		m="$(basename "$d")"
		shopt -s nullglob
		srcs=("$d"*.cpp)
		shopt -u nullglob
		[ ${#srcs[@]} -eq 0 ] && continue
		if $CXX -std=c++17 -fsyntax-only -Iinclude "${srcs[@]}" >/dev/null 2>&1; then
			if [ -z "${declared[$m]:-}" ]; then
				bad "$m compiles standalone but is missing from MODULES"
				status=1
			fi
		elif [ -n "${declared[$m]:-}" ]; then
			bad "$m is listed in MODULES but no longer compiles standalone"
			status=1
		fi
	done
	[ $status -eq 0 ] && ok "MODULES matches the tree"
	exit $status
fi

mkdir -p "$BUILD_DIR/obj" "$BUILD_DIR/lib" "$BUILD_DIR/bin"

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

	case "$kind" in
		test) flags="$SC_TESTFLAGS" ;;
		*)    flags="$SC_CXXFLAGS" ;;
	esac

	if [ -f "$obj" ] && [ "$obj" -nt "$src" ] && [ "$obj" -nt "$SC_SELF" ]; then
		return 0
	fi
	mkdir -p "$(dirname "$obj")"
	# shellcheck disable=SC2086
	if ! $CXX $flags -c "$src" -o "$obj" 2> "$obj.log"; then
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

for m in "${selected_modules[@]}"; do
	shopt -s nullglob
	for src in "src/$m"/*.cpp; do
		printf '%s\t%s\tmod\n' "$src" "$BUILD_DIR/obj/$m/$(basename "${src%.cpp}").o" >> "$JOBLIST"
	done
	shopt -u nullglob
done
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

# ── archive each module so the linker pulls only what a suite needs ──────────
for m in "${selected_modules[@]}"; do
	shopt -s nullglob
	objs=("$BUILD_DIR/obj/$m"/*.o)
	shopt -u nullglob
	[ ${#objs[@]} -eq 0 ] && continue
	ar rcs "$BUILD_DIR/lib/lib$m.a" "${objs[@]}"
done
ar rcs "$BUILD_DIR/lib/libgtest_lite_main.a" "$BUILD_DIR/obj/gtest_lite_main.o"

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

failed_suites=()
passed=0
for s in "${run_suites[@]}"; do
	shopt -s nullglob
	srcs=(tests/"$s"/*.cpp)
	shopt -u nullglob
	if [ ${#srcs[@]} -eq 0 ]; then
		skip "$s (no sources)"
		continue
	fi

	bin="$BUILD_DIR/bin/$s"
	# --start-group/--end-group resolves the undeclared cycles between module
	# archives without needing a topological order.
	if ! $CXX $TESTFLAGS "${srcs[@]}" \
			-Wl,--start-group "${libargs[@]}" "$BUILD_DIR/lib/libgtest_lite_main.a" -Wl,--end-group \
			"$BUILD_DIR/obj/gtest_lite.o" \
			-o "$bin" > "$bin.buildlog" 2>&1; then
		bad "$s (build)"
		head -25 "$bin.buildlog"
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
