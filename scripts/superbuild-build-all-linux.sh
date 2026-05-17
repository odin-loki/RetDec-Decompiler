#!/usr/bin/env bash
# Configure and build RetDec superbuild on Linux/WSL (native GCC): Debug + Release.
#
# Binary dirs:
#   build/linux/superbuild-debug
#   build/linux/superbuild-release
# Install prefixes:
#   install/linux/superbuild-debug
#   install/linux/superbuild-release
#
# Optional: also build MinGW Windows cross targets (Linux host only):
#   SUPERBUILD_MINGW=1 bash scripts/superbuild-build-all-linux.sh
#
# Optional: also build Clang release variant:
#   SUPERBUILD_CLANG=1 bash scripts/superbuild-build-all-linux.sh
#
# Usage (from repo root):
#   bash scripts/superbuild-build-all-linux.sh [--install]
set -euo pipefail

INSTALL=0
for a in "$@"; do
  [[ "$a" == "--install" ]] && INSTALL=1
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SUPER_SRC="$REPO_ROOT/cmake/superbuild"
JOBS="$(nproc)"

echo "Repository: $REPO_ROOT"
echo "Jobs: $JOBS"
echo ""

run_preset() {
  local p="$1"
  echo "=== cmake --preset $p (configure) ==="
  cmake -S "$SUPER_SRC" --preset "$p"
  local bd="$REPO_ROOT/build/linux/$p"
  echo "=== cmake --build $bd ==="
  cmake --build "$bd" --parallel "$JOBS"
  if [[ $INSTALL -eq 1 ]]; then
    echo "=== cmake --install $bd ==="
    cmake --install "$bd"
  fi
}

run_preset superbuild-debug
run_preset superbuild-release

if [[ "${SUPERBUILD_MINGW:-}" == "1" ]]; then
  echo ""
  echo "=== MinGW cross (Windows PE) ==="
  run_preset superbuild-windows-cross-mingw-debug
  run_preset superbuild-windows-cross-mingw
fi

if [[ "${SUPERBUILD_CLANG:-}" == "1" ]]; then
  echo ""
  echo "=== Linux Clang release ==="
  run_preset superbuild-linux-clang
fi

echo ""
echo "Done. Superbuild trees under build/linux/<preset>/"
