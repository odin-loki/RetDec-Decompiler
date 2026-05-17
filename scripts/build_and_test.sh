#!/usr/bin/env bash
# build_and_test.sh
# Run from project root in WSL.  Builds on WSL-native FS for speed, then
# cross-compiles a Windows PE, and copies it back for PowerShell testing.
#
# Usage (from repo root): bash scripts/build_and_test.sh [--skip-native] [--skip-windows]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SCRIPT_DIR/.." && pwd)"       # repository root
# Intentional out-of-tree build dirs (fast native FS); override if desired.
BUILD_NATIVE="${BUILD_NATIVE:-$HOME/retdec-build/core-debug}"
BUILD_WIN="${BUILD_WIN:-$HOME/retdec-build/win-cross}"
INSTALL_WIN="$HOME/retdec-install/win"
WIN_DEPLOY="$SRC/dist/windows"            # accessible from PowerShell
JOBS="$(nproc)"
TOOLCHAIN="$SRC/cmake/toolchains/windows-mingw-w64.cmake"

SKIP_NATIVE=0
SKIP_WINDOWS=0
for arg in "$@"; do
  case "$arg" in --skip-native)  SKIP_NATIVE=1 ;;
                 --skip-windows) SKIP_WINDOWS=1 ;;
  esac
done

log() { echo -e "\n\033[1;34m[$(date +%H:%M:%S)] $*\033[0m"; }
err() { echo -e "\033[1;31m[ERROR] $*\033[0m" >&2; }

# ─────────────────────────────────────────────────────────────────────────────
# Phase 1: Native Linux debug build + unit tests
# ─────────────────────────────────────────────────────────────────────────────
if [[ $SKIP_NATIVE -eq 0 ]]; then

  log "Phase 1: Configuring native debug build → $BUILD_NATIVE"
  mkdir -p "$BUILD_NATIVE"
  cmake \
    -S "$SRC" \
    -B "$BUILD_NATIVE" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DRETDEC_TESTS=ON \
    -DRETDEC_ENABLE_RETDEC=ON \
    -DRETDEC_ENABLE_RETDEC_DECOMPILER=ON \
    -DRETDEC_ENABLE_FILEFORMAT=ON \
    -DRETDEC_ENABLE_BIN2LLVMIR=ON \
    -DRETDEC_ENABLE_LLVMIR2HLL=ON \
    -DRETDEC_ENABLE_DEMANGLER=ON \
    -DRETDEC_ENABLE_COMMON=ON \
    -DRETDEC_ENABLE_CONFIG=ON \
    2>&1 | tee "$BUILD_NATIVE/cmake-configure.log"

  log "Phase 1: Building ($JOBS cores) …"
  cmake --build "$BUILD_NATIVE" -j"$JOBS" \
    2>&1 | tee "$BUILD_NATIVE/cmake-build.log"

  log "Phase 1: Running tests …"
  cd "$BUILD_NATIVE"
  ctest --output-on-failure -j"$JOBS" \
    2>&1 | tee "$BUILD_NATIVE/ctest.log"
  cd "$SRC"

  log "Phase 1: PASSED ✓"

fi

# ─────────────────────────────────────────────────────────────────────────────
# Phase 2: Windows cross-compile (MinGW-w64)
# ─────────────────────────────────────────────────────────────────────────────
if [[ $SKIP_WINDOWS -eq 0 ]]; then

  log "Phase 2: Configuring Windows cross-compile → $BUILD_WIN"
  mkdir -p "$BUILD_WIN" "$INSTALL_WIN"
  cmake \
    -S "$SRC" \
    -B "$BUILD_WIN" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_WIN" \
    -DRETDEC_TESTS=OFF \
    -DRETDEC_ENABLE_RETDEC=ON \
    -DRETDEC_ENABLE_RETDEC_DECOMPILER=ON \
    -DRETDEC_ENABLE_FILEFORMAT=ON \
    -DRETDEC_ENABLE_BIN2LLVMIR=ON \
    -DRETDEC_ENABLE_LLVMIR2HLL=ON \
    -DRETDEC_ENABLE_DEMANGLER=ON \
    -DRETDEC_ENABLE_COMMON=ON \
    -DRETDEC_ENABLE_CONFIG=ON \
    2>&1 | tee "$BUILD_WIN/cmake-configure.log"

  log "Phase 2: Building ($JOBS cores) …"
  cmake --build "$BUILD_WIN" -j"$JOBS" \
    2>&1 | tee "$BUILD_WIN/cmake-build.log"

  log "Phase 2: Installing → $INSTALL_WIN"
  cmake --install "$BUILD_WIN" \
    2>&1 | tee "$BUILD_WIN/cmake-install.log"

  log "Phase 2: Copying Windows binaries to $WIN_DEPLOY (accessible from PowerShell)"
  rm -rf "$WIN_DEPLOY"
  mkdir -p "$WIN_DEPLOY"
  # Copy all executables and their required MinGW runtime DLLs
  find "$INSTALL_WIN/bin" -name "*.exe" -exec cp -v {} "$WIN_DEPLOY/" \;
  find "$INSTALL_WIN/bin" -name "*.dll" -exec cp -v {} "$WIN_DEPLOY/" \;

  # Bundle required MinGW runtime DLLs (libstdc++, libgcc, libwinpthread)
  MINGW_SYSROOT="/usr/lib/gcc/x86_64-w64-mingw32/13-win32"
  for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    src_dll=$(find /usr/x86_64-w64-mingw32/lib/ /usr/lib/gcc/x86_64-w64-mingw32/ -name "$dll" 2>/dev/null | head -1)
    if [[ -f "$src_dll" ]]; then
      cp -v "$src_dll" "$WIN_DEPLOY/"
    fi
  done

  log "Phase 2: PASSED ✓"
  log "Windows binaries staged in: $WIN_DEPLOY"
  echo ""
  echo "  To test in PowerShell run:"
  WIN_PATH=$(echo "$WIN_DEPLOY" | sed 's|/mnt/c/|C:/|; s|/mnt/d/|D:/|; s|/|\\|g')
  echo "    & \"${WIN_PATH}\\retdec-decompiler.exe\" --help"
  echo ""

fi

log "All phases complete."
