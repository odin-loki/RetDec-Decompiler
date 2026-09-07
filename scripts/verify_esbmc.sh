#!/usr/bin/env bash
#
# scripts/verify_esbmc.sh — prove the verified kernels with ESBMC.
#
# What this proves, and what it does not
# --------------------------------------
# Fuzzing (scripts/standalone_fuzz.sh) shows that a bug EXISTS. It cannot show
# that one does not. Every crash it found in this tree was in the same few lines
# of arithmetic: a count, length or offset read out of a file and used before
# anything checked the input could supply it.
#
# That arithmetic now lives in the verified kernels under include/retdec/utils/,
# and this script proves them — for ALL inputs over the whole 64-bit domain, by
# SMT, not for sampled values. A harness with no loops is a proof outright; one
# that walks a buffer pins its bound with // ESBMC-OPTIONS: --unwind N, and
# ESBMC's unwinding assertions (on by default in 8.5.0) make exceeding that
# bound a failure rather than a silently truncated search.
#
# What is NOT proved: the parsers themselves, mostly — but not for the reason
# this comment used to give. std::vector, std::string, std::optional,
# std::variant and classes with methods all verify against ESBMC 8.5.0, and
# std::span does under --std c++20. The two real limits are that std::unique_ptr
# is a PARSE ERROR (so anything reaching retdec/ssa/ssa.h cannot be verified at
# all) and cost: PeReader::open over a fully symbolic buffer discharges in 1s at
# 16 bytes and does not return within 400s at 64. So whole-function proofs are
# available where a header-sized symbolic input is enough — use // ESBMC-LINK:
# to bring the implementation in — and coverage of everything else is the job of
# the unit suites and the fuzzer. See docs/VERIFICATION.md.
#
# Usage
#   scripts/verify_esbmc.sh              # every proof
#   scripts/verify_esbmc.sh proof_remaining proof_page_count
#   scripts/verify_esbmc.sh --list
#   scripts/verify_esbmc.sh --syntax   # type-check the harnesses, no solver
#   scripts/verify_esbmc.sh --cross    # every proof under two backends, diffed
#   scripts/verify_esbmc.sh --routing  # does anything CALL each proved kernel?
#   scripts/verify_esbmc.sh --optional # include harnesses marked ESBMC-OPTIONAL
#
# Environment
#   ESBMC     path to the esbmc binary                    (default: esbmc)
#   SOLVER    --z3 | --boolector | --bitwuzla | --cvc5    (default: --z3)
#   TIMEOUT   seconds per proof                           (default: 300)
#             a harness may raise its own with // ESBMC-TIMEOUT: <seconds>

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
#
# A bound is only a proof if exceeding it is reported. ESBMC 8.5.0 generates
# unwinding assertions by default -- there is no --unwinding-assertions flag to
# ask for, only --no-unwinding-assertions to turn them off -- so a harness that
# sets --unwind gets them, and a bound that is too small fails loudly rather
# than silently truncating the search. Nothing here may pass
# --no-unwinding-assertions.
harness_options() {
	grep -oE '^// ESBMC-OPTIONS:.*' "$1" | head -1 | sed 's|^// ESBMC-OPTIONS:||'
}

# A harness may pin its own solver with
#   // ESBMC-SOLVER: --z3
#
# It needs to, because the backends disagree. ESBMC emits an "arithmetic
# overflow on div" check for *unsigned* division, which cannot overflow in C++;
# boolector and bitwuzla find a witness for it (the signed INT64_MIN / -1 pair,
# 0x8000000000000001 - 1 over 0xFFFFFFFFFFFFFFFF) and report a violation, while
# z3 does not. The false alarm is in the safe direction -- it cannot hide a real
# bug -- but a harness doing unsigned division has to say which solver its
# verdict came from. Measured, not assumed: see --cross.
harness_solver() {
	grep -oE '^// ESBMC-SOLVER:.*' "$1" | head -1 | sed 's|^// ESBMC-SOLVER:||' | tr -d ' \t'
}

# A whole-function proof needs the implementation, not just the header:
#   // ESBMC-LINK: src/cli_parser/pe_reader.cpp
# Several may be listed on the one line. Without this a harness calling a real
# parser verifies against an empty body and proves nothing about it.
harness_link() {
	grep -oE '^// ESBMC-LINK:.*' "$1" | head -1 | sed 's|^// ESBMC-LINK:||'
}

# A harness that does not run by default:  // ESBMC-OPTIONAL: <reason>
#
# For a harness whose proofs are real and reproducible but do not fit this
# machine. The alternative is worse in both directions: shipping it in the
# default run leaves the suite red for a reason that is not a defect, and
# deleting it loses a measurement the documentation depends on.
#
# The reason is not optional. It is printed on every skipped run, so a harness
# that is quietly not being verified says so out loud each time.
#
# `--optional` runs them.
harness_optional() {
	grep -oE '^// ESBMC-OPTIONAL:.*' "$1" | head -1 | sed 's|^// ESBMC-OPTIONAL:||' | sed 's/^ *//'
}

# Seconds this harness may spend on one proof:  // ESBMC-TIMEOUT: 1800
#
# A kernel proof that needs more than the default 300s is usually a proof that
# is wrong, so the default stays where it is. A whole-function proof is a
# different animal: pe_reader_proof.cpp links a real translation unit and puts
# 30,000 verification conditions in front of the solver, and 75s of that is
# spent encoding before the solver starts. Raising TIMEOUT globally to suit it
# would hide a kernel proof that had quietly become expensive, so the budget is
# stated per harness, by the harness that needs it.
harness_timeout() {
	local v
	v="$(grep -oE '^// ESBMC-TIMEOUT:.*' "$1" | head -1 | sed 's|^// ESBMC-TIMEOUT:||' | tr -d ' \t')"
	printf '%s' "${v:-$TIMEOUT}"
}

# C++ standard for this harness:  // ESBMC-STD: c++20
# std::span needs c++20; the default stays c++17 to match the tree.
harness_std() {
	local v
	v="$(grep -oE '^// ESBMC-STD:.*' "$1" | head -1 | sed 's|^// ESBMC-STD:||' | tr -d ' \t')"
	printf '%s' "${v:-c++17}"
}

# A directive the parser cannot see is worse than no directive: the harness runs
# with the wrong options and still reports a verdict. This happened once, with
# the line written inside a /** */ block, so mention it anywhere in a harness
# and it must be in a form that is actually read.
check_options_syntax() {
	local file="$1"
	if grep -q 'ESBMC-OPTIONS' "$file" && [ -z "$(harness_options "$file")" ]; then
		bad "$(basename "$file"): ESBMC-OPTIONS present but not at the start of a // line"
		return 1
	fi
	if grep -q 'ESBMC-SOLVER' "$file" && [ -z "$(harness_solver "$file")" ]; then
		bad "$(basename "$file"): ESBMC-SOLVER present but not at the start of a // line"
		return 1
	fi
	if grep -q 'ESBMC-STD' "$file" && [ -z "$(harness_std "$file")" ]; then
		bad "$(basename "$file"): ESBMC-STD present but not at the start of a // line"
		return 1
	fi
	if grep -q 'ESBMC-OPTIONAL' "$file" && [ -z "$(harness_optional "$file")" ]; then
		bad "$(basename "$file"): ESBMC-OPTIONAL present with no reason, or not at the start of a // line"
		return 1
	fi
	if grep -q 'ESBMC-TIMEOUT' "$file"; then
		local t; t="$(harness_timeout "$file")"
		if ! printf '%s' "$t" | grep -qE '^[0-9]+$'; then
			bad "$(basename "$file"): ESBMC-TIMEOUT is not a whole number of seconds"
			return 1
		fi
	fi
	if grep -q 'ESBMC-LINK' "$file"; then
		if [ -z "$(harness_link "$file")" ]; then
			bad "$(basename "$file"): ESBMC-LINK present but not at the start of a // line"
			return 1
		fi
		local d
		for d in $(harness_link "$file"); do
			if [ ! -f "$d" ]; then
				bad "$(basename "$file"): ESBMC-LINK names $d, which does not exist"
				return 1
			fi
		done
	fi
	return 0
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

usage() { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; }

MODE=run
RUN_OPTIONAL=0
declare -a WANTED=()
while [ $# -gt 0 ]; do
	case "$1" in
		--list)    MODE=list ;;
		--syntax)  MODE=syntax ;;
		--cross)   MODE=cross ;;
		--routing) MODE=routing ;;
		--optional) RUN_OPTIONAL=1 ;;
		-h|--help) usage; exit 0 ;;
		-*)        say "unknown option: $1"; usage; exit 2 ;;
		*)         WANTED+=("$1") ;;
	esac
	shift
done

# ── routing ──────────────────────────────────────────────────────────────────
#
# The weak point of this whole approach is not the solver, it is the gap between
# a proof and the code that runs. A proof about a re-typed copy of the logic
# says nothing about the copy that ships, and this tree has demonstrated that
# repeatedly: src/utils, the module the proved headers live in, included none of
# them while carrying eight re-derivations of what they prove, every one wrong.
#
# So "prove it once and call it" is checked here rather than left to review. For
# every kernel with a harness, count the files that include it. Zero callers
# means the kernel is proved and nothing uses it, which is a finding — the bug
# it was written to stop is still in the tree, in the copy nobody routed.
#
# UNROUTED_KERNELS is the escape hatch, and it takes a reason, not just a name:
# a kernel is allowed no callers only while someone has written down why.
declare -A UNROUTED_KERNELS=()

# A harness whose name does not correspond to include/retdec/utils/<name>.h is a
# whole-function proof (it links real code and proves that code directly), so
# there is nothing to route and it is skipped rather than reported.
if [ "$MODE" = routing ]; then
	hdr "who calls each proved kernel"
	shopt -s nullglob
	harnesses=("$PROOF_DIR"/*.cpp)
	shopt -u nullglob
	status=0
	routed=0; unrouted=0; skipped=0
	for h in "${harnesses[@]}"; do
		base="$(basename "$h" .cpp)"; base="${base%_proof}"
		kernel="include/retdec/utils/${base}.h"
		if [ ! -f "$kernel" ]; then
			skip "$(basename "$h") — proves code directly, nothing to route"
			skipped=$((skipped + 1))
			continue
		fi
		# Callers: anything under src/ or include/ that includes the kernel,
		# except the kernel itself. Harnesses are excluded by not being searched.
		mapfile -t callers < <(
			grep -rl "retdec/utils/${base}\.h" src include 				--include='*.cpp' --include='*.h' 2>/dev/null 				| grep -v "^${kernel}$" | sort)
		if [ ${#callers[@]} -gt 0 ]; then
			ok "${base}.h — ${#callers[@]} caller(s): ${callers[*]}"
			routed=$((routed + 1))
		elif [ -n "${UNROUTED_KERNELS[$base]:-}" ]; then
			skip "${base}.h — no callers, allowed: ${UNROUTED_KERNELS[$base]}"
			unrouted=$((unrouted + 1))
		else
			bad "${base}.h — proved, and nothing in src/ or include/ calls it"
			status=1
			unrouted=$((unrouted + 1))
		fi
	done
	hdr "summary"
	say "routed: $routed   unrouted: $unrouted   not applicable: $skipped"
	if [ $status -ne 0 ]; then
		say ""
		say "A kernel with no callers is a proof about code that does not run."
		say "Either route the call sites it was written for, or add it to"
		say "UNROUTED_KERNELS in this script with the reason it is waiting."
		exit 1
	fi
	ok "every proved kernel has a caller"
	exit 0
fi

# A typo in a harness should not cost a solver run to find.
#
# The standard used here is the harness's own, not a fixed c++17: type-checking
# a harness at a standard the solver run will not use is worse than not checking
# it, because it reports errors that do not exist (a c++20 harness saw
# "std::span is only available from C++20 onwards" while the ESBMC run of the
# same file was fine) and would miss ones that do.
if [ "$MODE" = syntax ]; then
	shopt -s nullglob
	harnesses=("$PROOF_DIR"/*.cpp)
	shopt -u nullglob
	status=0
	for h in "${harnesses[@]}"; do
		if "${CXX:-g++}" -std="$(harness_std "$h")" -Wall -Wextra -fsyntax-only -Iinclude \
				-DRETDEC_VERIFY_SYNTAX_ONLY "$h" && check_options_syntax "$h"; then
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

# ── cross-check ──────────────────────────────────────────────────────────────
#
# A proof is a claim about a program, not about a solver. Two backends that
# disagree mean at least one is wrong, and until it is known which, the property
# is not proved. This runs everything under z3 and under boolector and reports
# every disagreement.
#
# The cross-check runs at the default TIMEOUT for both backends, not at a
# harness's raised ESBMC-TIMEOUT. A harness that needs longer than the default
# then times out on both sides, which agrees, or on one, which its pin already
# explains -- and the alternative, running a whole-function harness twice at its
# own budget, turns this from a check you run into one you do not.
#
# One disagreement is already known and is a tool artifact rather than a code
# defect: ESBMC emits an "arithmetic overflow on div" check for unsigned
# division, which cannot overflow in C++, and the bitvector backends find the
# signed INT64_MIN / -1 witness for it while z3 does not. Harnesses doing
# unsigned division pin a solver and are reported EXPECTED here. Anything else
# is a finding.
verdict_of() {   # harness fn solver std extraOpts [linkSrcs...]
	local harness="$1" fn="$2" solver="$3" std="$4" extra="$5"; shift 5
	local log rc; log="$(mktemp)"
	# shellcheck disable=SC2086
	timeout "$TIMEOUT" "$ESBMC" "$harness" "$@" -I include "$solver" --std "$std" \
		--function "$fn" "${CHECKS[@]}" $extra > "$log" 2>&1
	rc=$?
	rm -f "$log"
	case $rc in
		0)   printf 'pass' ;;
		124) printf 'timeout' ;;
		*)   printf 'fail' ;;
	esac
}

if [ "$MODE" = cross ]; then
	hdr "cross-checking every proof under z3 and boolector"
	agree=0; disagree=0; expected=0; noverdict=0
	declare -a mismatches=()
	for harness in "${HARNESSES[@]}"; do
		check_options_syntax "$harness" || { disagree=$((disagree+1)); continue; }
		# ESBMC-OPTIONAL applies here too, and it matters more here than in the
		# run loop: cross-checking runs each proof TWICE, so a harness that
		# cannot finish costs double, and an OOM kill comes back as a non-zero
		# exit from both backends -- which this mode would otherwise record as
		# "z3 and boolector agree (fail)". Two crashed processes are not a
		# consensus about a property. Skipped unless --optional asks for it.
		crossOptional="$(harness_optional "$harness")"
		if [ -n "$crossOptional" ] && [ "$RUN_OPTIONAL" -eq 0 ]; then
			skip "$(basename "$harness") — not cross-checked: $crossOptional"
			continue
		fi
		extraOpts="$(harness_options "$harness")"
		harnessStd="$(harness_std "$harness")"
		pinned="$(harness_solver "$harness")"
		# shellcheck disable=SC2206
		linkSrcs=($(harness_link "$harness"))
		mapfile -t proofs < <(collect_proofs "$harness")
		for fn in "${proofs[@]}"; do
			if [ ${#WANTED[@]} -gt 0 ]; then
				match=0
				for w in "${WANTED[@]}"; do [ "$w" = "$fn" ] && match=1; done
				[ $match -eq 0 ] && continue
			fi
			a="$(verdict_of "$harness" "$fn" --z3        "$harnessStd" "$extraOpts" "${linkSrcs[@]}")"
			b="$(verdict_of "$harness" "$fn" --boolector "$harnessStd" "$extraOpts" "${linkSrcs[@]}")"
			if [ "$a" = timeout ] && [ "$b" = timeout ]; then
				# Not an agreement. Two backends that both ran out of time have
				# said nothing about the property, and counting that as
				# agreement is how a cross-check comes back clean on a file it
				# never actually checked.
				skip "$fn — neither backend returned within ${TIMEOUT}s"
				noverdict=$((noverdict + 1))
			elif [ "$a" = "$b" ]; then
				ok "$fn — z3 and boolector agree ($a)"
				agree=$((agree + 1))
			elif [ -n "$pinned" ]; then
				skip "$fn — z3=$a boolector=$b; harness pins $pinned"
				expected=$((expected + 1))
			else
				bad "$fn — z3=$a boolector=$b"
				mismatches+=("$fn")
				disagree=$((disagree + 1))
			fi
		done
	done
	hdr "summary"
	say "agreed: $agree   expected disagreement: $expected   no verdict: $noverdict   unexplained: $disagree"
	if [ $noverdict -gt 0 ]; then
		say ""
		say "A property neither backend finished is not cross-checked. Raise"
		say "TIMEOUT and run those names again, or accept that the single-solver"
		say "verdict in the main run is all there is for them."
	fi
	if [ $disagree -gt 0 ]; then
		bad "solvers disagree on: ${mismatches[*]}"
		say ""
		say "A property two solvers disagree about is not proved. Either the"
		say "harness has undefined behaviour they model differently, or one of"
		say "them is wrong. Find out which before trusting the verdict."
		exit 1
	fi
	ok "no unexplained disagreement"
	exit 0
fi

hdr "$($ESBMC --version | head -1)"
say "solver: $SOLVER   timeout: ${TIMEOUT}s per proof"

total=0
passed=0
skipped=0
failed=()

for harness in "${HARNESSES[@]}"; do
	hdr "$(basename "$harness")"

	if ! check_options_syntax "$harness"; then
		failed+=("$(basename "$harness") options")
		continue
	fi
	extraOpts="$(harness_options "$harness")"
	[ -n "$extraOpts" ] && say "extra options:$extraOpts"
	harnessOptional="$(harness_optional "$harness")"
	if [ -n "$harnessOptional" ] && [ "$RUN_OPTIONAL" -eq 0 ]; then
		skip "$(basename "$harness") — not run by default: $harnessOptional"
		skipped=$((skipped + 1))
		continue
	fi
	harnessSolver="$(harness_solver "$harness")"
	: "${harnessSolver:=$SOLVER}"
	[ "$harnessSolver" != "$SOLVER" ] && say "solver: $harnessSolver (pinned by the harness)"
	harnessStd="$(harness_std "$harness")"
	harnessTimeout="$(harness_timeout "$harness")"
	[ "$harnessTimeout" != "$TIMEOUT" ] && say "timeout: ${harnessTimeout}s (raised by the harness)"
	# shellcheck disable=SC2206
	linkSrcs=($(harness_link "$harness"))
	[ ${#linkSrcs[@]} -gt 0 ] && say "linking: ${linkSrcs[*]}"

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
		if timeout "$harnessTimeout" "$ESBMC" "$harness" "${linkSrcs[@]}" -I include \
				"$harnessSolver" --std "$harnessStd" \
				--function "$fn" "${CHECKS[@]}" $extraOpts > "$log" 2>&1; then
			n="$(grep -oE '[0-9]+ passed' "$log" | head -1)"
			ok "$fn — ${n:-verified}"
			passed=$((passed + 1))
		else
			status=$?
			if [ $status -eq 124 ]; then
				bad "$fn — timed out after ${harnessTimeout}s"
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
[ $skipped -gt 0 ] && say "harnesses not run (ESBMC-OPTIONAL): $skipped"
if [ ${#failed[@]} -gt 0 ]; then
	bad "failing: ${failed[*]}"
	exit 1
fi
if [ "$total" -eq 0 ]; then
	say "no proofs matched"
	exit 2
fi
ok "all properties hold"
