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
	"ci-smoke  cmake sources:::bash scripts/check_cmake_sources.sh"
	"ci-smoke  preset cache leaks (self-test):::${PY} scripts/ci/check_cmake_presets.py --self-test"
	"ci-smoke  preset cache leaks:::${PY} scripts/ci/check_cmake_presets.py CMakePresets.json cmake/superbuild/CMakePresets.json"
	"ci-smoke  unread build options (self-test):::${PY} scripts/ci/check_cmake_options.py --self-test"
	"ci-smoke  unread build options:::${PY} scripts/ci/check_cmake_options.py"
	"ci-smoke  std algorithm includes:::bash scripts/check_std_includes.sh"
	"ci-smoke  retdec CLI:::${PY} scripts/python/test_retdec_cli.py"
	"ci-smoke  semantic C hints:::${PY} tests/decompiler/semantic_c_hint_test.py"
	"ci-smoke  pipeline validation:::${PY} tests/decompiler/validate_pipeline_test.py"
	"ci-smoke  semantic detection JSON:::${PY} tests/decompiler/semantic_detection_json_test.py"
	"ci-smoke  Intel export:::${PY} tests/decompiler/intel_export_test.py"
	"ci-smoke  algorithm-recovery labels:::${PY} tests/algorithm_recovery/test_labels.py"
	"ci-smoke  algorithm-recovery gate:::${PY} tests/algorithm_recovery/test_regression_gate.py"
	"ci-smoke  algorithm-recovery corpus:::${PY} tests/algorithm_recovery/test_corpus_resolve.py"
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
)

if [ "${1:-}" = "--list" ]; then
	for entry in "${CHECKS[@]}"; do
		printf '%s\n' "${entry%%:::*}"
	done
	exit 0
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
