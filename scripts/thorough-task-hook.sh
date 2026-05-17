#!/usr/bin/env bash
# Thorough gate for Taskmaster tasks: exit 0 only when checks pass.
# Usage: thorough-task-hook.sh <taskId>
#
# Task 1–2: real repo / CI checks (CMake superbuild, cross-compile artifacts).
# Task 3+: provide an agent (required for this hook to pass):
#   CODING_AGENT=/path/to/script.sh  — called as: script.sh <taskId>  (preferred)
#   or CODING_AGENT_CMD='snippet'    — run via bash -c; TASK_ID and REPO_ROOT are exported first
#
# Optional:
#   SUPERBUILD_PRESET=superbuild-debug  (default)
#   Superbuild presets live in cmake/superbuild/CMakePresets.json (CMake 4+); use -S cmake/superbuild.
#   SKIP_CMAKE_BUILD=1                  — configure only for task 1

set -euo pipefail

TASK_ID="${1:-}"
if [ -z "$TASK_ID" ]; then
	echo "usage: $0 <taskId>" >&2
	exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

SUPERBUILD_PRESET="${SUPERBUILD_PRESET:-superbuild-debug}"

run_cmake_superbuild() {
	if ! command -v cmake >/dev/null 2>&1; then
		echo "task $TASK_ID: cmake not on PATH — install CMake 3.26+ and retry." >&2
		return 1
	fi
	local super_src="${REPO_ROOT}/cmake/superbuild"
	local super_build
	case "$(uname -s 2>/dev/null)" in
		MINGW*|MSYS*|CYGWIN*) super_build="${REPO_ROOT}/build/windows/${SUPERBUILD_PRESET}" ;;
		*) super_build="${REPO_ROOT}/build/linux/${SUPERBUILD_PRESET}" ;;
	esac
	echo "task $TASK_ID: cmake -S superbuild --preset $SUPERBUILD_PRESET"
	cmake -S "${super_src}" --preset "$SUPERBUILD_PRESET"
	if [ -n "${SKIP_CMAKE_BUILD:-}" ]; then
		echo "task $TASK_ID: SKIP_CMAKE_BUILD=1 — skipping build step."
		return 0
	fi
	echo "task $TASK_ID: cmake --build ${super_build}"
	cmake --build "${super_build}" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-}"
}

verify_cross_compile_artifacts() {
	local missing=0
	for f in \
		"cmake/toolchains/windows-mingw-w64.cmake" \
		"cmake/toolchains/linux-clang.cmake" \
		"vcpkg.json" \
		"CMakePresets.json" \
		"cmake/superbuild/CMakePresets.json"; do
		if [ ! -f "$REPO_ROOT/$f" ]; then
			echo "task $TASK_ID: missing required file: $f" >&2
			missing=1
		fi
	done
	# CI workflow references superbuild / MinGW smoke (best-effort)
	if ! grep -q "superbuild" "$REPO_ROOT/.github/workflows/mingw-cross-smoke.yml" 2>/dev/null; then
		echo "task $TASK_ID: expected superbuild references in .github/workflows/mingw-cross-smoke.yml" >&2
		missing=1
	fi
	return "$missing"
}

invoke_agent() {
	export TASK_ID REPO_ROOT
	if [ -n "${CODING_AGENT:-}" ]; then
		echo "task $TASK_ID: running CODING_AGENT=$CODING_AGENT"
		if [ -f "$CODING_AGENT" ]; then
			if [ ! -x "$CODING_AGENT" ]; then
				echo "CODING_AGENT is not executable: $CODING_AGENT" >&2
				return 1
			fi
			"$CODING_AGENT" "$TASK_ID"
			return $?
		fi
		if command -v "$CODING_AGENT" >/dev/null 2>&1; then
			"$CODING_AGENT" "$TASK_ID"
			return $?
		fi
		echo "CODING_AGENT not found or not executable: $CODING_AGENT" >&2
		return 1
	fi
	if [ -z "${CODING_AGENT_CMD:-}" ]; then
		echo "task $TASK_ID: no automated gate in this hook." >&2
		echo "Set CODING_AGENT to an executable script that receives the task id as argv[1], or set CODING_AGENT_CMD." >&2
		echo "Example:  export CODING_AGENT=\"$REPO_ROOT/scripts/example-coding-agent.sh\"" >&2
		return 1
	fi
	echo "task $TASK_ID: running CODING_AGENT_CMD (bash -c snippet)"
	bash -c "$CODING_AGENT_CMD"
}

case "$TASK_ID" in
1)
	run_cmake_superbuild
	;;
2)
	verify_cross_compile_artifacts
	;;
*)
	invoke_agent
	;;
esac
