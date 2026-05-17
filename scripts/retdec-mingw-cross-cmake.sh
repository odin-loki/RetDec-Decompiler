#!/usr/bin/env bash
# Configure a RetDec MinGW (Windows PE) cross-build from WSL or Linux.
# See docs/README.md — "Linux / WSL → Windows PE (MinGW cross-compilation)".
#
# Usage:
#   ./scripts/retdec-mingw-cross-cmake.sh [BUILD_DIR] [-- extra cmake args...]
#   ./scripts/retdec-mingw-cross-cmake.sh -h|--help
# Env:
#   TOOLCHAIN   — default: <repo>/cmake/toolchains/windows-mingw-gcc.cmake
#                 (set to .../windows-llvm-mingw.cmake for llvm-mingw / clang)
#   CMAKE_BUILD_TYPE — default: Release
#
set -euo pipefail
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
	cat <<'EOF'
retdec-mingw-cross-cmake.sh — configure RetDec for MinGW (Windows PE) from WSL/Linux.

Usage:
  ./scripts/retdec-mingw-cross-cmake.sh [BUILD_DIR] [-- extra cmake args...]

  BUILD_DIR defaults to build-mingw-w64. Use "--" first for default dir + extra flags.

Environment:
  TOOLCHAIN          Path to toolchain file (default: cmake/toolchains/windows-mingw-gcc.cmake)
  CMAKE_BUILD_TYPE   Default: Release

Optional (after this script): re-run cmake on the MinGW build dir with
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
and set CCACHE_DIR (see docs/MINGW_CROSS_DEEP_DIVE.md — CI uses .ccache-mingw-smoke).

Example (llvm-mingw):
  TOOLCHAIN="$PWD/cmake/toolchains/windows-llvm-mingw.cmake" ./scripts/retdec-mingw-cross-cmake.sh

See docs/README.md (Linux / WSL → Windows PE).
EOF
	exit 0
fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" == "--" ]]; then
	BUILD_DIR="build-mingw-w64"
else
	BUILD_DIR="${1:-build-mingw-w64}"
	shift || true
fi
TOOLCHAIN="${TOOLCHAIN:-$ROOT/cmake/toolchains/windows-mingw-gcc.cmake}"
BT="${CMAKE_BUILD_TYPE:-Release}"

if [[ "${1:-}" == "--" ]]; then
	shift
fi

mkdir -p "$ROOT/$BUILD_DIR"
echo "==> RetDec MinGW cross configure"
echo "    Source:  $ROOT"
echo "    Build:   $ROOT/$BUILD_DIR"
echo "    Toolchain: $TOOLCHAIN"
echo "    Build type: $BT"
cmake -S "$ROOT" -B "$ROOT/$BUILD_DIR" \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
	-DCMAKE_BUILD_TYPE="$BT" \
	"$@"
echo "==> Next: cmake --build $ROOT/$BUILD_DIR -j\$(nproc)"
