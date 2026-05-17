#!/usr/bin/env bash
# Two-stage MinGW cross-build: native Linux → llvm-tblgen, then Windows (MinGW) configure.
# Complements **scripts/retdec-mingw-cross-cmake.sh** when you do not already have **build-wsl/**.
#
# Full maintainer notes: **docs/MINGW_CROSS_DEEP_DIVE.md**
#
# Prerequisites (Debian/Ubuntu examples):
#   sudo apt-get install cmake ninja-build python3 g++ g++-mingw-w64-x86-64 perl make
#
# Usage:
#   ./scripts/mingw-cross-two-stage.sh
#   ./scripts/mingw-cross-two-stage.sh my-host-build my-mingw-build
#
# Environment:
#   RETDEC_LLVM_TABLEGEN — if set and executable, skip stage 1
#   SKIP_HOST_STAGE=1    — skip stage 1 (requires RETDEC_LLVM_TABLEGEN)
#   HOST_BUILD           — default: $REPO/build-host-llvm-tblgen
#   MINGW_BUILD          — directory name under $REPO (default: build-mingw-w64)
#   JOBS                 — parallel jobs (default: nproc or 4)
#   CMAKE_BUILD_TYPE     — default: Release
#   TOOLCHAIN            — passed to retdec-mingw-cross-cmake.sh (MinGW toolchain file)
#   HOST_CMAKE_EXTRA     — extra arguments for stage-1 **cmake** (quoted string)
#
# Optional **ccache** (host + pass through to stage-2 via cmake — set the same launchers on MinGW **cmake**):
#   export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/retdec-ccache}"
#   mkdir -p "$CCACHE_DIR"; ccache -M 2G 2>/dev/null || true
#   export HOST_CMAKE_EXTRA='-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache …'
#   # After **retdec-mingw-cross-cmake.sh**, re-run **cmake** on the MinGW build dir with the same LAUNCHER flags if needed.
#
# Stage 1 default is a **full** RetDec host configure (RETDEC_ENABLE_ALL=ON) — slow but reliable.
# For a leaner host build, try (may need extra -D flags if configure fails):
#   HOST_CMAKE_EXTRA='-DRETDEC_ENABLE_ALL=OFF -DRETDEC_ENABLE_BIN2LLVMIR=ON'
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${1:-}" ]]; then
	_host="${1}"
	[[ "$_host" == /* ]] || _host="$ROOT/$_host"
	HOST_BUILD="$_host"
else
	HOST_BUILD="${HOST_BUILD:-$ROOT/build-host-llvm-tblgen}"
	[[ "$HOST_BUILD" == /* ]] || HOST_BUILD="$ROOT/$HOST_BUILD"
fi
if [[ -n "${2:-}" ]]; then
	MINGW_DIRNAME="$(basename "$2")"
else
	MINGW_DIRNAME="$(basename "${MINGW_BUILD:-build-mingw-w64}")"
fi
# MINGW_DIRNAME is a single path component under ROOT (same as retdec-mingw-cross-cmake.sh)
JOBS="${JOBS:-$(nproc 2>/dev/null || true)}"
JOBS="${JOBS:-4}"
BT="${CMAKE_BUILD_TYPE:-Release}"

if [[ "${SKIP_HOST_STAGE:-0}" == "1" || "${SKIP_HOST_STAGE:-0}" == "true" ]]; then
	if [[ -z "${RETDEC_LLVM_TABLEGEN:-}" || ! -x "${RETDEC_LLVM_TABLEGEN}" ]]; then
		echo "SKIP_HOST_STAGE=1 requires an executable RETDEC_LLVM_TABLEGEN" >&2
		exit 1
	fi
	TBLGEN="$RETDEC_LLVM_TABLEGEN"
	echo "==> Skipping host stage; using RETDEC_LLVM_TABLEGEN=$TBLGEN"
else
	if [[ -n "${RETDEC_LLVM_TABLEGEN:-}" && -x "${RETDEC_LLVM_TABLEGEN}" ]]; then
		TBLGEN="$RETDEC_LLVM_TABLEGEN"
		echo "==> Using existing RETDEC_LLVM_TABLEGEN=$TBLGEN (skip host build)"
	else
		echo "==> Stage 1: host build for llvm-tblgen → $HOST_BUILD"
		mkdir -p "$HOST_BUILD"
		# shellcheck disable=SC2086
		cmake -S "$ROOT" -B "$HOST_BUILD" -DCMAKE_BUILD_TYPE="$BT" ${HOST_CMAKE_EXTRA:-}
		cmake --build "$HOST_BUILD" --target llvm-project -j"$JOBS"
		TBLGEN="$HOST_BUILD/external/src/llvm-project-build/bin/llvm-tblgen"
		if [[ ! -x "$TBLGEN" ]]; then
			echo "error: expected host llvm-tblgen at: $TBLGEN" >&2
			exit 1
		fi
		echo "==> Host llvm-tblgen: $TBLGEN"
	fi
fi

echo "==> Stage 2: MinGW cross configure ($MINGW_DIRNAME)"
export RETDEC_LLVM_TABLEGEN="$TBLGEN"
RETDEC_LLVM_TABLEGEN="$TBLGEN" \
	"$ROOT/scripts/retdec-mingw-cross-cmake.sh" "$MINGW_DIRNAME"

echo "==> Done. Build with:"
echo "    cmake --build $ROOT/$MINGW_DIRNAME -j$JOBS"
