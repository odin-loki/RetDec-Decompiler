#!/usr/bin/env bash
#
# scripts/standalone_fuzz.sh — fuzz the untrusted-input parsers without LLVM.
#
# Why this exists
# ---------------
# tests/managed_integration/fuzz/ has had libFuzzer harnesses for a while, but
# they are gated behind -DRETDEC_FUZZ=ON, and fuzz_elf links retdec::fileformat,
# which PUBLIC-links LLVM. So enabling the option builds the pinned LLVM fork:
# hours. As a result .github/workflows/fuzz-pr.yml can only check on a pull
# request that the harness *files exist*.
#
# Most of the harnesses do not need any of that. The .pyc, .luac, .wasm, .dex,
# .class, .jar/.apk, .pdb and CIL parsers are all in the dependency-free set
# (see scripts/standalone_check.sh), so they can be built and fuzzed with
# clang++ and nothing else. Those parsers are exactly the code that reads
# attacker-controlled bytes.
#
# Usage
#   scripts/standalone_fuzz.sh --replay          # regression only, seconds (CI)
#   scripts/standalone_fuzz.sh                   # replay, then fuzz each target
#   scripts/standalone_fuzz.sh pyc wasm          # only these targets
#   scripts/standalone_fuzz.sh --list
#   scripts/standalone_fuzz.sh --time 300        # seconds per target
#
# --replay runs every seed and every checked-in reproducer under
# tests/crash_corpus/ and exits. It is deterministic and fast, which is what a
# pull request wants; discovery belongs on a schedule.
#
# A new crash is written to tests/crash_corpus/<target>/ so it becomes a
# permanent regression case.
#
# Environment
#   CXX           compiler, must support -fsanitize=fuzzer  (default: clang++)
#   FUZZ_TIME     seconds per target                        (default: 60)
#   MAX_LEN       largest input libFuzzer will generate      (default: 65536)
#   JOBS          parallel build jobs                       (default: nproc)
#   BUILD_DIR     build output                              (default: build/fuzz)
#   CORPUS_DIR    persistent corpus                         (default: build/fuzz/corpus)

set -u -o pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CXX="${CXX:-clang++}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
BUILD_DIR="${BUILD_DIR:-build/fuzz}"
CORPUS_DIR="${CORPUS_DIR:-$BUILD_DIR/corpus}"
FUZZ_TIME="${FUZZ_TIME:-60}"

# libFuzzer defaults to 4096, and that default hides whole bug classes rather
# than merely slowing their discovery. A marshal container costs two bytes per
# nesting level, so 4 KB caps nesting at about 2000 -- comfortably survivable --
# and the stack overflow a 30 KB file triggers is unreachable no matter how long
# the fuzzer runs. Replay uses the same value so a large committed reproducer is
# not silently skipped.
MAX_LEN="${MAX_LEN:-65536}"

readonly HARNESS_DIR="tests/managed_integration/fuzz"
readonly CRASH_DIR="tests/crash_corpus"

# target : harness stem : src modules : C++ standard : link libs : seed glob
readonly TARGETS=(
	"pyc:fuzz_pyc:pyc_parser bc_module:17::tests/managed_integration/fixtures/python/*.pyc"
	"lua:fuzz_lua:lua_parser bc_module:17::tests/managed_integration/fixtures/lua/*.luac"
	"wasm:fuzz_wasm:wasm_parser bc_module:17::tests/managed_integration/fixtures/wasm/*.wasm"
	"dex:fuzz_dex:dex_parser bc_module:17:-lz:tests/managed_integration/fixtures/dex/*.dex"
	"jvm:fuzz_jvm_class:jvm_parser bc_module:17:-lz:tests/managed_integration/fixtures/java/*.class"
	"jar:fuzz_jar:jvm_parser bc_module:17:-lz:tests/managed_integration/fixtures/java/*.jar"
	"apk:fuzz_apk:dex_parser bc_module:17:-lz:tests/managed_integration/fixtures/dex/*.apk"
	"pdb:fuzz_pdb:pdbparser:17::tests/managed_integration/fixtures/pdb/*.pdb"
	"cil:fuzz_cli:cli_parser bc_module:20:-lz:tests/managed_integration/fixtures/dotnet/*.dll"
)

# Malformed fixtures make excellent seeds: they already sit on the error paths.
readonly MALFORMED_ROOT="tests/managed_integration/fixtures/malformed"

INCLUDES="-Iinclude -Ideps/rapidjson/include"
DEFINES="-DRAPIDJSON_HAS_STDSTRING=1"
SAN="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"

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

usage() { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; }

MODE=full
declare -a WANTED=()
while [ $# -gt 0 ]; do
	case "$1" in
		--replay)  MODE=replay ;;
		--list)    MODE=list ;;
		--build)   MODE=build ;;
		--time)    shift; FUZZ_TIME="${1:-60}" ;;
		-h|--help) usage; exit 0 ;;
		-*)        say "unknown option: $1"; usage; exit 2 ;;
		*)         WANTED+=("$1") ;;
	esac
	shift
done

field() { printf '%s' "$1" | cut -d: -f"$2"; }

if [ "$MODE" = list ]; then
	say "targets:"
	for t in "${TARGETS[@]}"; do
		printf '  %-6s %-16s modules: %s\n' "$(field "$t" 1)" "$(field "$t" 2)" "$(field "$t" 3)"
	done
	exit 0
fi

if ! $CXX -fsanitize=fuzzer -x c++ /dev/null -c -o /dev/null >/dev/null 2>&1; then
	say "$CXX cannot build libFuzzer targets."
	say "On Debian/Ubuntu the runtime lives in the matching libclang-rt-<N>-dev package."
	exit 2
fi

mkdir -p "$BUILD_DIR/bin" "$CORPUS_DIR"

selected=()
for t in "${TARGETS[@]}"; do
	name="$(field "$t" 1)"
	if [ ${#WANTED[@]} -eq 0 ]; then
		selected+=("$t")
	else
		for w in "${WANTED[@]}"; do [ "$w" = "$name" ] && selected+=("$t"); done
	fi
done
if [ ${#selected[@]} -eq 0 ]; then
	say "no matching targets (see --list)"
	exit 2
fi

# ── build ───────────────────────────────────────────────────────────────────
hdr "building ${#selected[@]} fuzz target(s) with $CXX"

build_one() {
	local spec="$1"
	local name harness modules std libs
	name="$(printf '%s' "$spec" | cut -d: -f1)"
	harness="$(printf '%s' "$spec" | cut -d: -f2)"
	modules="$(printf '%s' "$spec" | cut -d: -f3)"
	std="$(printf '%s' "$spec" | cut -d: -f4)"
	libs="$(printf '%s' "$spec" | cut -d: -f5)"

	local src="$HARNESS_DIR/$harness.cpp"
	if [ ! -f "$src" ]; then
		printf 'MISSING %s\n' "$name"
		return 0
	fi

	local srcs=("$src")
	local m
	for m in $modules; do
		shopt -s nullglob
		srcs+=("src/$m"/*.cpp)
		shopt -u nullglob
	done

	# shellcheck disable=SC2086
	if $CXX -std=c++$std $INCLUDES $DEFINES $SAN -g -O1 "${srcs[@]}" $libs \
			-o "$BUILD_DIR/bin/$name" > "$BUILD_DIR/bin/$name.log" 2>&1; then
		printf 'BUILT %s\n' "$name"
	else
		printf 'BROKEN %s\n' "$name"
	fi
}
export -f build_one
export CXX BUILD_DIR HARNESS_DIR INCLUDES DEFINES SAN

buildOut="$(printf '%s\n' "${selected[@]}" | xargs -d '\n' -P "$JOBS" -n 1 bash -c 'build_one "$0"')"

declare -a runnable=()
for t in "${selected[@]}"; do
	name="$(field "$t" 1)"
	case "$buildOut" in
		*"BUILT $name"*) ok "built $name"; runnable+=("$t") ;;
		*"MISSING $name"*)
			# A target listed in TARGETS whose harness has gone missing is a
			# hole in the gate, not something to skip past: the whole point of
			# this workflow existing is that fuzz_dex.cpp stopped compiling and
			# a presence check did not notice. Delete the row to retire a
			# target deliberately.
			bad "$name: no harness at $HARNESS_DIR/$(field "$t" 2).cpp"
			say "  remove the row from TARGETS to retire this target on purpose"
			exit 1
			;;
		*)
			bad "$name (build)"
			head -20 "$BUILD_DIR/bin/$name.log" 2>/dev/null
			exit 1
			;;
	esac
done

[ "$MODE" = build ] && exit 0

# ── seed ────────────────────────────────────────────────────────────────────
seed_corpus() {
	local name="$1" glob="$2"
	local dir="$CORPUS_DIR/$name"
	mkdir -p "$dir"

	shopt -s nullglob
	local seeds=($glob)
	# Malformed fixtures start on the error paths, which is where the bugs are.
	local mal
	for mal in "$MALFORMED_ROOT"/*/*; do
		[ -f "$mal" ] && seeds+=("$mal")
	done
	shopt -u nullglob

	local f
	for f in "${seeds[@]}"; do
		[ -f "$f" ] || continue
		cp -n "$f" "$dir/$(basename "$f")" 2>/dev/null || true
	done
	printf '%s' "$dir"
}

# ── replay ──────────────────────────────────────────────────────────────────
hdr "replaying seeds and known reproducers"

replayFailed=()
ungated=()
for t in "${runnable[@]}"; do
	name="$(field "$t" 1)"
	glob="$(field "$t" 6)"
	dir="$(seed_corpus "$name" "$glob")"

	inputs=("$dir")
	[ -d "$CRASH_DIR/$name" ] && inputs+=("$CRASH_DIR/$name")

	count=$(find "${inputs[@]}" -type f 2>/dev/null | wc -l)
	if [ "$count" -eq 0 ]; then
		# Replaying nothing proves nothing. Report it rather than printing a
		# green line, so a target whose seeds have gone missing is visible.
		skip "$name: no seeds and no reproducers -- this target is not gated"
		ungated+=("$name")
		continue
	fi

	# -malloc_limit_mb matters as much as -rss_limit_mb here: an allocation the
	# parser makes because the file *claimed* a huge count shows up as one
	# oversized malloc, not as steadily growing RSS, so without this a replay of
	# an out-of-memory reproducer passes and the whole OOM class goes ungated.
	if out="$("$BUILD_DIR/bin/$name" "${inputs[@]}" -runs=0 \
			-max_len="$MAX_LEN" \
			-rss_limit_mb=2048 \
			-malloc_limit_mb=512 2>&1)"; then
		ok "$name replayed $count input(s)"
	else
		bad "$name (replay)"
		printf '%s\n' "$out" | grep -E "ERROR|SUMMARY|#[0-9]+ .*retdec" | head -15
		replayFailed+=("$name")
	fi
done

if [ ${#replayFailed[@]} -gt 0 ]; then
	hdr "summary"
	bad "replay failures: ${replayFailed[*]}"
	exit 1
fi

if [ "$MODE" = replay ]; then
	hdr "summary"
	if [ ${#ungated[@]} -gt 0 ]; then
		skip "no inputs for: ${ungated[*]}"
	fi
	ok "replay clean (${#runnable[@]} target(s), $(( ${#runnable[@]} - ${#ungated[@]} )) with inputs)"
	exit 0
fi

# ── fuzz ────────────────────────────────────────────────────────────────────
hdr "fuzzing ${#runnable[@]} target(s) for ${FUZZ_TIME}s each"

found=()
for t in "${runnable[@]}"; do
	name="$(field "$t" 1)"
	dir="$CORPUS_DIR/$name"
	mkdir -p "$CRASH_DIR/$name"

	# artifact_prefix puts any reproducer straight into the crash corpus, so a
	# find survives the run and becomes a regression case.
	if out="$("$BUILD_DIR/bin/$name" "$dir" \
			-max_total_time="$FUZZ_TIME" \
			-max_len="$MAX_LEN" \
			-rss_limit_mb=2048 \
			-malloc_limit_mb=512 \
			-artifact_prefix="$CRASH_DIR/$name/" \
			-print_final_stats=1 2>&1)"; then
		execs="$(printf '%s' "$out" | grep -oE 'number_of_executed_units: [0-9]+' | grep -oE '[0-9]+')"
		ok "$name — ${execs:-0} execs, no findings"
	else
		bad "$name — new finding"
		printf '%s\n' "$out" | grep -E "ERROR|SUMMARY|Test unit written|#[0-9]+ .*retdec" | head -15
		found+=("$name")
	fi
done

hdr "summary"
if [ ${#found[@]} -gt 0 ]; then
	bad "new findings in: ${found[*]}"
	say "reproducers saved under $CRASH_DIR/<target>/ — commit them as regression cases"
	exit 1
fi
ok "no new findings"
