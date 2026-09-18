#!/usr/bin/env bash
# check_push_gates.sh - every check a push runs, run here first.
#
# Why this exists
# ---------------
# ci-smoke and doc-integrity are lists of scripts in this repository. All of
# them run in seconds and need no build, so there is no reason to learn from CI
# what they would have said -- and yet two pushes on this branch went red on
# exactly that: a `std::remove` that check_std_includes.sh reads as the
# <algorithm> one, and a documentation table that check_release_binaries.py
# read as a list of release artefacts.
#
# This runs them, in the order the workflows do, and reports which failed. It is
# not a substitute for the workflows -- they also build, and standalone-check
# and the format check are separate and slower -- it is the part that is cheap.
#
# The list has to be every step, not almost every step. It first shipped
# missing two -- ci-smoke's inline debug_enabled grep, which is written in the
# workflow rather than in a script, and doc-integrity's REL-06 CycloneDX step
# -- so a push that broke either of them still read "all checks pass" here and
# went red in CI, which is the exact failure this script exists to prevent.
# It then shipped missing a third, the clang-format check, on a note claiming
# it had "its own workflow". It does not: ci-smoke runs it, and a push whose
# added lines were unformatted read "all 37 checks pass" here and went red
# there. Running the --self-test is not running the check.
#
# The list ends with one check that is not from those two workflows and is not
# cheap: a clang++ compile of every standalone module. The warning baseline is
# the union over three configurations -- g++, clang++, and g++ with the
# sanitizers -- so a local g++ run cannot tell whether removing a baseline
# entry is safe. One was removed on that evidence and standalone-check went red
# on `src/debug_info/pdb_extractor.cpp -Wunused-const-variable`, a warning only
# clang emits. It reuses its own object cache, so the first run costs minutes
# and the rest cost seconds.
#
# What CI diffs on a push is the previously pushed commit, so that is what the
# entry below asks for -- @{upstream}. A pull request diffs the merge base
# with the default branch instead, which is a larger question and a different
# answer; `bash scripts/check_format.sh` with no argument is that one.
# --audit compares the list against the workflows and is the thing that keeps
# them from drifting apart again.
#
# Usage:
#   bash scripts/check_push_gates.sh          # everything below
#   bash scripts/check_push_gates.sh --list   # name them and exit
#
# Exit status is 0 when every check passes, 1 otherwise.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PY="${PYTHON:-python3}"

# name:::command. Kept in workflow order so a failure here lines up with the
# step that would have reported it.
CHECKS=(
	"ci-smoke  clang-format (pushed range):::bash scripts/check_format.sh --base \"\$(git rev-parse --verify -q '@{upstream}' || git rev-parse HEAD^)\""
	"ci-smoke  cmake sources:::bash scripts/check_cmake_sources.sh"
	# PORT-01 runs on Linux and fails on a defect only Windows could otherwise
	# report -- and only after a build that takes hours. `z` resolves here by
	# accident.
	"ci-smoke  no bare Unix library names:::${PY} scripts/ci/check_portable_link_names.py"
	"ci-smoke  bare Unix lib names (self-test):::${PY} scripts/ci/check_portable_link_names.py --self-test"
	# PORT-02 is the same shape: standard C++ that gcc and clang accept and
	# MSVC rejects with a syntax cascade naming neither the token nor the cause.
	"ci-smoke  no alternative operator tokens:::${PY} scripts/ci/check_alternative_tokens.py"
	"ci-smoke  alternative tokens (self-test):::${PY} scripts/ci/check_alternative_tokens.py --self-test"
	"ci-smoke  preset cache leaks (self-test):::${PY} scripts/ci/check_cmake_presets.py --self-test"
	"ci-smoke  preset cache leaks:::${PY} scripts/ci/check_cmake_presets.py CMakePresets.json cmake/superbuild/CMakePresets.json"
	"ci-smoke  unread build options (self-test):::${PY} scripts/ci/check_cmake_options.py --self-test"
	"ci-smoke  unread build options:::${PY} scripts/ci/check_cmake_options.py"
	"ci-smoke  no header defines debug_enabled:::! grep -rqn '^[[:space:]]*\\(const[[:space:]]\\+\\)\\?bool[[:space:]]\\+debug_enabled' include/"
	"ci-smoke  std algorithm includes:::bash scripts/check_std_includes.sh"
	"ci-smoke  retdec CLI:::${PY} scripts/python/test_retdec_cli.py"
	"ci-smoke  semantic C hints:::${PY} tests/decompiler/semantic_c_hint_test.py"
	"ci-smoke  pipeline validation:::${PY} tests/decompiler/validate_pipeline_test.py"
	"ci-smoke  semantic detection JSON:::${PY} tests/decompiler/semantic_detection_json_test.py"
	"ci-smoke  Intel export:::${PY} tests/decompiler/intel_export_test.py"
	"ci-smoke  algorithm-recovery labels:::${PY} tests/algorithm_recovery/test_labels.py"
	"ci-smoke  algorithm-recovery gate:::${PY} tests/algorithm_recovery/test_regression_gate.py"
	"ci-smoke  algorithm-recovery gate wiring:::bash scripts/ci/check_recovery_gate_wiring.sh"
	"ci-smoke  algorithm-recovery corpus:::${PY} tests/algorithm_recovery/test_corpus_resolve.py"
	"ci-smoke  decompile failure summary:::${PY} tests/algorithm_recovery/test_failure_summary.py"
	"ci-smoke  triton gate:::${PY} tests/algorithm_recovery/test_triton_gate.py"
	"ci-smoke  ship checklist:::bash scripts/ship_checklist.sh"
	"ci-smoke  doctor:::bash scripts/doctor.sh"
	"doc       link graph (self-test):::${PY} scripts/ci/check_link_graph.py --self-test"
	"doc       docs vs code:::${PY} scripts/ci/check_doc_vs_code.py"
	"doc       relative links (self-test):::${PY} scripts/ci/check_doc_links.py --self-test"
	"doc       relative links:::${PY} scripts/ci/check_doc_links.py"
	"doc       withdrawn claims:::${PY} scripts/ci/check_withdrawn_claims.py"
	"doc       Avast MIT notice:::${PY} scripts/ci/check_avast_mit_notice.py"
	"doc       ELF hardening (self-test):::${PY} scripts/ci/check_elf_hardening.py --self-test"
	"doc       clang-tidy (self-test):::${PY} scripts/ci/run_clang_tidy.py --self-test"
	"doc       keystone not installed:::${PY} scripts/ci/check_keystone_not_installed.py"
	"doc       Qt dynamic link:::${PY} scripts/ci/check_qt_dynamic_link.py"
	"doc       provenance summary:::${PY} scripts/ci/generate_provenance.py"
	"doc       version drift:::${PY} scripts/ci/check_version_drift.py"
	"doc       release binaries:::${PY} scripts/ci/check_release_binaries.py"
	"doc       secrets:::${PY} scripts/ci/check_secrets.py"
	"doc       CycloneDX pins:::${PY} scripts/ci/generate_cyclonedx.py --out \"$(mktemp)\""
	"doc       VERIFICATION.md vs proofs:::bash scripts/verify_esbmc.sh --doc"
	"doc       output determinism (self-test):::bash scripts/ci/check_output_determinism.sh --self-test"
	"doc       clang-format scoping (self-test):::bash scripts/check_format.sh --self-test"
	"doc       emitted C compiles (self-test):::bash scripts/ci/check_emitted_c_compiles.sh --self-test"
	"doc       CLI exit codes (self-test):::bash scripts/ci/check_cli_exit_codes.sh --self-test"
	"doc       non-x86 decompile (self-test):::bash scripts/ci/check_multiarch_decompile.sh --self-test"
	"standalone clang++ compile + warnings:::CXX=clang++ BUILD_DIR=build/standalone-clang bash scripts/standalone_check.sh --compile-only"
	"standalone module/suite audit:::bash scripts/standalone_check.sh --audit"
	"standalone llvm_to_ssa adapter (system LLVM):::bash scripts/ci/check_llvm_adapter.sh"
	"standalone llvmir2hll suite (system LLVM):::bash scripts/ci/check_llvmir2hll_tests.sh"
	"standalone sem_decoder suite (system Capstone):::bash scripts/ci/check_sem_decoder_tests.sh"
	"standalone demangler suite (system LLVM):::bash scripts/ci/check_demangler_tests.sh"
	"standalone fileformat suites (system LLVM):::bash scripts/ci/check_fileformat_tests.sh"
	"doc       calling conventions (self-test):::${PY} scripts/ci/check_calling_conventions.py --self-test"
	"doc       calling conventions:::${PY} scripts/ci/check_calling_conventions.py"
	"standalone arity tables vs headers:::${PY} scripts/ci/check_libc_arity.py --check"
	"standalone opencl library and kernels:::bash scripts/ci/check_opencl_tests.sh --self-test"
	"standalone capstone2llvmir (5 architectures):::bash scripts/ci/check_capstone2llvmir_tests.sh --self-test"
	# OPT-01. bin2llvmir had no gate of any kind: the layer that rewrites the
	# lifted IR was checked only by IR-text comparisons for one of its passes.
	# This evaluates each rewrite before and after over a domain of inputs and
	# requires the same answer, which is the question the text comparison was
	# never asking.
	"standalone bin2llvmir rewrites (semantics):::bash scripts/ci/check_bin2llvmir_opts.sh --self-test"
	"standalone idiom exchangers vs PHI:::bash scripts/ci/check_idiom_phi_reach.sh --self-test"
	"standalone idiom rewrites vs other uses:::bash scripts/ci/check_idiom_shared_use.sh --self-test"
	"standalone bin2llvmir suites (system LLVM):::bash scripts/ci/check_bin2llvmir_tests.sh --workdir \"${B2L_WORKDIR:-$ROOT/build/b2l-cache}\" --jobs 4"
	"standalone suites ctest never runs:::bash scripts/ci/check_orphan_test_suites.sh --b2l-workdir \"${B2L_WORKDIR:-$ROOT/build/b2l-cache}\""
	# PIN-01 is the only check here that compiles against the LLVM
	# cmake/deps.cmake actually pins. Every other one takes the machine's
	# llvm-config -- 18 or 20 locally, 20 in standalone-check -- and a commit
	# that compiled on 20 and not on 23 sat red in ctest-linux for a day and a
	# half. The first run fetches the pinned archive and builds its
	# tablegen-generated headers, minutes and about 3 GiB; after that it stamps
	# what it checked and a clean re-run costs under a second. It needs a
	# configured build directory for its compile_commands.json and says so.
	"standalone tree vs the pinned LLVM:::${PY} scripts/ci/check_pinned_llvm.py --workdir \"${PIN_WORKDIR:-$ROOT/build/pin-cache}\" --jobs 4"
	"standalone pinned LLVM (self-test):::${PY} scripts/ci/check_pinned_llvm.py --workdir \"${PIN_WORKDIR:-$ROOT/build/pin-cache}\" --self-test"
	"standalone llvmir2hll warnings:::bash scripts/ci/check_llvmir2hll_warnings.sh --jobs 4"
	# BOUND-01. if_to_switch_optimizer.cpp is 8,100 lines, of which some six
	# thousand are forty near-identical compare-tree reconstructions written
	# by copying. A copy carries its overflow guard with it, including into
	# the mirror that needs the other extreme -- which is what happened to
	# all six bounds of the six-level Ge function. Reading forty near-identical
	# functions is the task a reader does badly and a differential does well.
	"doc       int64 bound overflow guards:::${PY} scripts/ci/check_bound_guards.py --self-test"
	# Runs after the entry above so that the capstone 5.0.9 it needs is already
	# built. COV-01 itself wants a cross-compiled corpus and minutes, so what
	# runs here is its self-test -- which is more than ran before, since
	# nothing anywhere invoked this script at all.
	"standalone instruction coverage (self-test):::${PY} scripts/ci/check_instruction_coverage.py --self-test --capstone-prefix \"${C2L_DEPS_DIR:-/tmp/c2l-deps}/capstone-install\""
	# PSEUDO-01 asks the translator rather than the table: which instructions
	# come out as a pseudo-assembly call. Its self-test builds the probe, which
	# means the translator sources have to compile and link against system LLVM
	# without the test suite -- worth knowing on its own -- and then checks that
	# the probe answers "not pseudo" for nop and "__asm_cpuid" for cpuid. The
	# measurement over a corpus runs in standalone-check, which has the cross
	# toolchains.
	"standalone pseudo-asm probe (self-test):::bash scripts/ci/check_pseudo_asm.sh --self-test --capstone-prefix \"${C2L_DEPS_DIR:-/tmp/c2l-deps}/capstone-install\""
	# Needs only LLVM's headers, so it belongs here rather than behind a build.
	"standalone IR2HLL opcode coverage:::${PY} scripts/ci/check_ir2hll_opcodes.py"
	"standalone IR2HLL opcodes (self-test):::${PY} scripts/ci/check_ir2hll_opcodes.py --self-test"
	"ci-smoke  no new unread option fields:::${PY} scripts/ci/check_unread_options.py --check"
	"ci-smoke  every workflow parses:::${PY} scripts/ci/check_workflow_yaml.py"
	"ci-smoke  workflow parse check (self-test):::${PY} scripts/ci/check_workflow_yaml.py --self-test"
)

if [ "${1:-}" = "--list" ]; then
	for entry in "${CHECKS[@]}"; do
		printf '%s\n' "${entry%%:::*}"
	done
	exit 0
fi

# --audit: does CHECKS still cover what the two workflows run?
#
# The list is hand-written, so it can fall behind a workflow that gains a step
# -- and it already had, by two. This reads every script the workflow files
# invoke and reports any that no entry above runs. Steps that are not a script
# call (the inline debug_enabled grep) cannot be found this way and are listed
# by name below, which is at least a place to notice them.
#
# Some workflow scripts deliberately are not here: they need a build, a
# network fetch, or minutes rather than seconds. WORKFLOW_ONLY says which, and
# saying so is the point -- an unexplained omission is what this catches.
WORKFLOW_ONLY=(
	"scripts/build_algorithm_corpus.sh"           # needs a compiler and minutes
	"scripts/fetch-large-files.sh"                # network
	"scripts/fetch_decompilebench_corpus.sh"      # network
	"scripts/run_benchmarks.sh"                   # needs a built decompiler
)

# The audit ran in one direction only -- workflow step with no gate here -- and
# the other direction was where the hole was. Three gates added on this branch
# (OPT-01, BOUND-01, IDIOM-PHI-01) ran ONLY from this script, and no workflow
# invokes this script: ctest-linux.yml names it in a comment and nothing else
# does. So they were enforced exactly when somebody remembered to run them by
# hand, which is not enforcement. They are wired into standalone-check.yml and
# doc-integrity.yml now, and the reverse check below keeps the next one from
# repeating it.
#
# LOCAL_ONLY is the escape hatch, and like WORKFLOW_ONLY it exists to make the
# omission say why.
LOCAL_ONLY=(
	"scripts/check_format.sh"                     # CI diffs the merge base, not @{upstream}
	"scripts/check_push_gates.sh"                 # this script
	"scripts/standalone_check.sh"                 # run by standalone-check.yml with its own flags
	# ctest-linux.yml builds these same sources against this same pin, so CI
	# does enforce it; what CI does not have is a way to say so in seconds
	# rather than in a half-hour build, and it needs a configured build
	# directory that the standalone jobs do not create.
	"scripts/ci/check_pinned_llvm.py"             # ctest-linux.yml compiles the same tree against the same pinned LLVM
)

if [ "${1:-}" = "--audit" ]; then
	status=0
	listed="$(printf '%s\n' "${CHECKS[@]}")"
	for wf in .github/workflows/ci-smoke.yml .github/workflows/doc-integrity.yml; do
		[ -f "$wf" ] || continue
		while read -r script; do
			[ -n "$script" ] || continue
			[ -f "$script" ] || continue
			skip=""
			for only in "${WORKFLOW_ONLY[@]}"; do
				[ "$only" = "$script" ] && skip=1
			done
			[ -n "$skip" ] && continue
			if ! printf '%s' "$listed" | grep -qF -- "$script"; then
				echo "check_push_gates --audit: $wf runs $script, which no check above does"
				status=1
			fi
		done < <(grep -oE '(bash|python3?) +[A-Za-z0-9_./-]+\.(sh|py)' "$wf" | awk '{print $2}' | sort -u)
	done

	# The other direction: a gate here that no workflow runs is a gate CI does
	# not enforce.
	ALL_WF="$(cat .github/workflows/*.yml 2>/dev/null)"
	for entry in "${CHECKS[@]}"; do
		name="${entry%%:::*}"
		cmd="${entry#*:::}"
		for script in $(printf '%s' "$cmd" \
				| grep -oE '(scripts|tests)/[A-Za-z0-9_./-]+\.(sh|py)' | sort -u); do
			[ -f "$script" ] || continue
			skip=""
			for only in "${LOCAL_ONLY[@]}"; do
				[ "$only" = "$script" ] && skip=1
			done
			[ -n "$skip" ] && continue
			# grep -qF <<< rather than printf | grep -qF: with `set -o
			# pipefail`, grep -q exits the moment it matches, printf takes a
			# SIGPIPE, and the pipeline reports 141 -- so a MATCH reads as a
			# failure. It only bit when the match was far from the end of the
			# 150k buffer, which is why the first run of this check reported
			# 51 gates as unrun including several plainly in a workflow.
			if ! grep -qF -- "$script" <<< "$ALL_WF"; then
				echo "check_push_gates --audit: '$name' runs $script, which no workflow does"
				status=1
			fi
		done
	done

	if [ $status -eq 0 ]; then
		echo "check_push_gates --audit: workflows and gates cover each other"
	fi
	exit $status
fi

failed=()
for entry in "${CHECKS[@]}"; do
	name="${entry%%:::*}"
	cmd="${entry#*:::}"
	printf '%-46s ' "$name"
	if out="$(eval "$cmd" 2>&1)"; then
		echo "ok"
	else
		echo "FAIL"
		printf '%s\n' "$out" | tail -12 | sed 's/^/      /'
		failed+=("$name")
	fi
done

echo
if [ ${#failed[@]} -gt 0 ]; then
	echo "check_push_gates: ${#failed[@]} of ${#CHECKS[@]} failed:"
	for name in "${failed[@]}"; do
		echo "  $name"
	done
	exit 1
fi

# generate_provenance.py rewrites docs/PROVENANCE-files.md in place, and that
# file is checked in, so a run that changes it means the committed copy is
# stale -- which is a failure of this script's own purpose if it goes unsaid.
if ! git diff --quiet -- docs/PROVENANCE-files.md 2>/dev/null; then
	echo "check_push_gates: docs/PROVENANCE-files.md changed; commit the regenerated file"
	exit 1
fi

echo "check_push_gates: all ${#CHECKS[@]} checks pass"
