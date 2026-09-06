#!/usr/bin/env bash
#
# scripts/verify_esbmc.sh — prove the bounds arithmetic with ESBMC.
#
# What this proves, and what it does not
# --------------------------------------
# Fuzzing (scripts/standalone_fuzz.sh) shows that a bug EXISTS. It cannot show
# that one does not. Every crash it found in this tree was in the same few lines
# of arithmetic: a count, length or offset read out of a file and used before
# anything checked the input could supply it.
#
# That arithmetic now lives in one place, include/retdec/utils/bounds.h, and
# this script proves it — for ALL inputs over the whole 64-bit domain, by SMT,
# not for sampled values. The harnesses in tests/verification/ have no loops, so
# no unwinding bound applies and the results are proofs, not bounded searches.
#
# What is NOT proved: the parsers themselves. ESBMC's operational models of the
# C++ standard library do not stretch to this codebase (std::variant is capped
# at four alternatives, std::min has no initializer_list overload), so whole-
# module verification is not available. The value here is that the arithmetic
# every parser depends on is proved once, and the parsers call it rather than
# re-deriving it. Coverage of the call sites is the job of the unit suites and
# the fuzzer.
#
# Usage
#   scripts/verify_esbmc.sh              # every proof
#   scripts/verify_esbmc.sh proof_remaining proof_page_count
#   scripts/verify_esbmc.sh --list
#   scripts/verify_esbmc.sh --syntax   # type-check the harnesses, no solver
#
# Environment
#   ESBMC     path to the esbmc binary          (default: esbmc on PATH)
#   SOLVER    --z3 | --boolector | --bitwuzla   (default: --z3)
#   TIMEOUT   seconds per proof                 (default: 300)

set -u -o pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ESBMC="${ESBMC:-esbmc}"
SOLVER="${SOLVER:---z3}"
TIMEOUT="${TIMEOUT:-300}"

readonly PROOF_DIR="tests/verification"

# Checks ESBMC applies to every harness. --overflow-check and
# --unsigned-overflow-check matter most: the bug class being guarded against is
# arithmetic that wraps, so the proof has to fail if any of it does.
readonly CHECKS=(
	--overflow-check
	--unsigned-overflow-check
	--ub-shift-check
	--nan-check
	--memory-leak-check
)

# A harness may add its own flags with a line of the form
#   // ESBMC-OPTIONS: --unwind 14 --unwinding-assertions
# Loop-bearing harnesses need an unwind bound, and it belongs next to the code
# whose bound it is rather than in a table here that can drift out of step.
# Without --unwinding-assertions a bound is an assumption, not a proof, so a
# harness that sets one is expected to ask for the assertions too.
harness_options() {
	grep -oE '^// ESBMC-OPTIONS:.*' "$1" | head -1 | sed 's|^// ESBMC-OPTIONS:||'
}

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

usage() { sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; }

MODE=run
declare -a WANTED=()
while [ $# -gt 0 ]; do
	case "$1" in
		--list)    MODE=list ;;
		--syntax)  MODE=syntax ;;
		-h|--help) usage; exit 0 ;;
		-*)        say "unknown option: $1"; usage; exit 2 ;;
		*)         WANTED+=("$1") ;;
	esac
	shift
done

# A typo in a harness should not cost a solver run to find.
if [ "$MODE" = syntax ]; then
	shopt -s nullglob
	harnesses=("$PROOF_DIR"/*.cpp)
	shopt -u nullglob
	status=0
	for h in "${harnesses[@]}"; do
		if "${CXX:-g++}" -std=c++17 -Wall -Wextra -fsyntax-only -Iinclude \
				-DRETDEC_VERIFY_SYNTAX_ONLY "$h"; then
			ok "$(basename "$h")"
		else
			bad "$(basename "$h")"
			status=1
		fi
	done
	exit $status
fi

if ! command -v "$ESBMC" >/dev/null 2>&1; then
	say "esbmc not found."
	say ""
	say "It is not packaged in Debian or Ubuntu. Take the release build:"
	say "  curl -fsSL -o esbmc-linux.zip \\"
	say "    https://github.com/esbmc/esbmc/releases/latest/download/esbmc-linux.zip"
	say "  unzip -q esbmc-linux.zip && chmod +x release/bin/esbmc"
	say "  export ESBMC=\$PWD/release/bin/esbmc"
	exit 2
fi

# Every `proof_*` entry point across the harnesses. Discovering them from the
# source rather than listing them here means a new proof is picked up by adding
# the function, and cannot be forgotten.
collect_proofs() {
	local file="$1"
	grep -oE '^extern "C" void (proof_[A-Za-z0-9_]+)' "$file" \
		| sed 's/^extern "C" void //'
}

shopt -s nullglob
HARNESSES=("$PROOF_DIR"/*.cpp)
shopt -u nullglob

if [ ${#HARNESSES[@]} -eq 0 ]; then
	say "no harnesses under $PROOF_DIR"
	exit 2
fi

if [ "$MODE" = list ]; then
	for h in "${HARNESSES[@]}"; do
		say "$h:"
		collect_proofs "$h" | sed 's/^/  /'
	done
	exit 0
fi

hdr "$($ESBMC --version | head -1)"
say "solver: $SOLVER   timeout: ${TIMEOUT}s per proof"

total=0
passed=0
failed=()

for harness in "${HARNESSES[@]}"; do
	hdr "$(basename "$harness")"

	extraOpts="$(harness_options "$harness")"
	[ -n "$extraOpts" ] && say "extra options:$extraOpts"

	mapfile -t proofs < <(collect_proofs "$harness")
	if [ ${#proofs[@]} -eq 0 ]; then
		skip "$(basename "$harness") declares no proof_* entry points"
		continue
	fi

	for fn in "${proofs[@]}"; do
		if [ ${#WANTED[@]} -gt 0 ]; then
			match=0
			for w in "${WANTED[@]}"; do [ "$w" = "$fn" ] && match=1; done
			[ $match -eq 0 ] && continue
		fi

		total=$((total + 1))
		log="$(mktemp)"

		# Each proof is verified on its own so a counterexample names the
		# property that broke rather than the file.
		# shellcheck disable=SC2086
		if timeout "$TIMEOUT" "$ESBMC" "$harness" -I include "$SOLVER" \
				--function "$fn" "${CHECKS[@]}" $extraOpts > "$log" 2>&1; then
			n="$(grep -oE '[0-9]+ passed' "$log" | head -1)"
			ok "$fn — ${n:-verified}"
			passed=$((passed + 1))
		else
			status=$?
			if [ $status -eq 124 ]; then
				bad "$fn — timed out after ${TIMEOUT}s"
			else
				bad "$fn"
				# The counterexample is the useful part of the output.
				grep -E "Violated property|line [0-9]+ assertion|^\s+assert|Counterexample|State [0-9]+" \
					"$log" | head -20
			fi
			failed+=("$fn")
		fi
		rm -f "$log"
	done
done

hdr "summary"
say "proofs passed: $passed / $total"
if [ ${#failed[@]} -gt 0 ]; then
	bad "failing: ${failed[*]}"
	exit 1
fi
if [ "$total" -eq 0 ]; then
	say "no proofs matched"
	exit 2
fi
ok "all properties hold"
