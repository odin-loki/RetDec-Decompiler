#!/bin/sh
# Local runner for ci/github/ci.yml when GitHub Actions is unavailable (no docker, so no nektos/act).
# Each job mirrors the workflow's steps with user-local toolchains instead of apt/brew/msys2.
#   ci/run-local.sh [job...]   jobs: linux-gcc linux-clang linux-tsan linux-asan shells windows-mingw
#                                    freebsd android macos ios     (default: all)
# Toolchain locations are overridable: LLVM, GXX_PREFIX, MINGW_TC, WINE, ZIG, NDK, SHELL_DIRS, FBSD_VM_SH.
# Results: $CI_OUT/<job>.log and a summary line per job in $CI_OUT/summary.txt.
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
CI_ROOT=${CI_ROOT:-/workspace/ci-local}; CI_OUT=${CI_OUT:-$CI_ROOT/logs}; J=${J:-6}
LLVM=${LLVM:-/workspace/opt/llvm18/bin}
GXX=${GXX:-$LLVM/x86_64-conda-linux-gnu-g++}; GCC=${GCC:-$LLVM/x86_64-conda-linux-gnu-gcc}
MINGW_TC=${MINGW_TC:-/workspace/mingw.cmake}
WINE=${WINE:-/workspace/opt/wine-11.20-amd64-wow64/bin/wine}
ZIG=${ZIG:-$(python3 -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'zig'))" 2>/dev/null || command -v zig)}
NDK=${NDK:-/workspace/opt/android-ndk-r27c}
SHELL_DIRS=${SHELL_DIRS:-/workspace/opt/shells/bin:/workspace/opt/xshells/bin}
export CTEST_OUTPUT_ON_FAILURE=1 FORGE_THREADS=2
mkdir -p "$CI_OUT"; cd "$src"

job_linux() { # $1 = c compiler, $2 = c++ compiler, $3 = build dir
  cmake -S . -B "$3" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="$1" -DCMAKE_CXX_COMPILER="$2" &&
  cmake --build "$3" -j"$J" &&
  ctest --test-dir "$3" -LE "slow|emu" -j2 --timeout 1200; }
job_linux_gcc()   { job_linux "$GCC" "$GXX" "$CI_ROOT/linux-gcc"; }
job_linux_clang() { job_linux "$LLVM/clang" "$LLVM/clang++" "$CI_ROOT/linux-clang"; }
job_san() { # $1 = FORGE_TSAN|FORGE_SANITIZE, $2 = dir, $3 = build type
  cmake -S . -B "$2" -G Ninja -DCMAKE_BUILD_TYPE="$3" -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
    -D"$1"=ON -DFORGE_BUILD_EXAMPLES=OFF &&
  cmake --build "$2" -j"$J" --target test_script forge &&
  setarch "$(uname -m)" -R "$2/test_script"; }  # -R replaces the workflow's sysctl vm.mmap_rnd_bits=28 (no root here)
job_linux_tsan() { TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" job_san FORGE_TSAN "$CI_ROOT/tsan" RelWithDebInfo; }
job_linux_asan() { job_san FORGE_SANITIZE "$CI_ROOT/asan" Debug; }
job_shells() {
  for s in sh bash zsh dash busybox ksh ksh93 mksh oksh yash osh ysh hush fish tcsh csh posh rc es elvish nu xonsh pwsh; do
    p=$(PATH="$SHELL_DIRS:$PATH" command -v $s 2>/dev/null) && echo "shell $s: $p" || echo "shell $s: MISSING"; done
  cmake -S . -B "$CI_ROOT/shells" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="$GCC" -DCMAKE_CXX_COMPILER="$GXX" \
    -DFORGE_BUILD_EXAMPLES=OFF &&
  cmake --build "$CI_ROOT/shells" -j"$J" --target test_script forge &&
  PATH="$SHELL_DIRS:$PATH" FORGE_TEST_ONLY=emit "$CI_ROOT/shells/test_script"; }
winepath_w() { "$(dirname "$WINE")/winepath" -w "$1" 2>/dev/null || echo "Z:$(echo "$1" | tr / '\\\\')"; }
job_windows_mingw() { # windows-mingw job: llvm-mingw cross build, ctest via wine; cmd/pwsh emit goldens.
  b="$CI_ROOT/win64"; export WINEPREFIX=${WINEPREFIX:-$CI_ROOT/wineprefix} WINEDEBUG=-all
  [ -d "$WINEPREFIX" ] || xvfb-run -a "$WINE" wineboot -i >/dev/null 2>&1
  # wine's powershell.exe is a stub that prints nothing and real PowerShell 7 crashes under wine: hide the stub so
  # pwsh/powershell report "not installed". busybox-w32 stands in for Git-for-Windows' sh on the hosted runner.
  ps="$WINEPREFIX/drive_c/windows/system32/WindowsPowerShell/v1.0/powershell.exe"; [ -f "$ps" ] && mv "$ps" "$ps.stub"
  wb=${WIN_SH_DIR:-$CI_ROOT/winbin}; [ -f "$wb/sh.exe" ] && export WINEPATH="$(winepath_w "$wb")"
  cmake -S . -B "$b" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$MINGW_TC" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFORGE_BUILD_EXAMPLES=OFF \
    -DCMAKE_CROSSCOMPILING_EMULATOR="$WINE" &&
  cmake --build "$b" -j"$J" &&
  # python-driven tests launch forge.exe directly from Linux python (no wine in between): run natively by linux-*.
  xvfb-run -a ctest --test-dir "$b" -LE "slow|emu" -E "^(golden_|modelfile_signing|onnx_import|docs_up_to_date|sbom_spdx|doc_example_inference_generate)" \
    -j2 --timeout 1200 &&
  FORGE_TEST_ONLY=emit xvfb-run -a "$WINE" "$b/test_script.exe"; }   # from the source dir (tests/data/emit)
job_freebsd() { # needs FBSD_VM_SH: a script that boots the VM, syncs $src, and runs the workflow's cmake/ctest steps inside.
  [ -n "${FBSD_VM_SH:-}" ] && [ -x "$FBSD_VM_SH" ] || { echo "SKIP: set FBSD_VM_SH (see ci/freebsd-vm.sh)"; return 77; }
  "$FBSD_VM_SH" "$src"; }
job_android() { # NDK cross builds + host --emit android goldens (emulator part only when adb has a device).
  export ANDROID_NDK_HOME="$NDK" FORGE_MOBILE_BUILD_DIR="$CI_ROOT/mobile"
  sh tests/mobile_build.sh android arm64-v8a &&
  cmake -S . -B "$CI_ROOT/android-x86_64" -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-arm64.cmake -DANDROID_ABI=x86_64 \
    -DCMAKE_BUILD_TYPE=Release -DFORGE_BUILD_EXAMPLES=OFF &&
  cmake --build "$CI_ROOT/android-x86_64" -j"$J" --target forge test_script &&
  if command -v adb >/dev/null && adb get-state >/dev/null 2>&1; then
    h="$CI_ROOT/linux-gcc"; [ -x "$h/forge" ] || h="$CI_ROOT/shells"; mkdir -p emit
    for g in functions flat batch; do "$h/forge" script --emit android tests/data/emit/$g.cpp > emit/$g.sh
      "$h/forge" script tests/data/emit/$g.cpp > emit/$g.want; echo $? > emit/$g.rc; done
    ANDROID_NDK_LATEST_HOME="$NDK" sh .github/android_check.sh; r=$?; rm -rf emit out.txt; return $r
  else echo "no adb device: emulator step not run"; fi; }
job_zig_cross() { # macos/ios: no Apple SDK here, so zig cross compile/link checks (ctest -L cross) stand in.
  [ -x "$ZIG" ] || { echo "SKIP: no zig"; return 77; }
  b="$CI_ROOT/cross"; [ -f "$b/CTestTestfile.cmake" ] ||
    cmake -S . -B "$b" -G Ninja -DCMAKE_C_COMPILER="$GCC" -DCMAKE_CXX_COMPILER="$GXX" -DFORGE_BUILD_EXAMPLES=OFF "-DFORGE_ZIG=$ZIG" >/dev/null
  ctest --test-dir "$b" -L cross -R "$1" -j"$J" --timeout 1200; }
job_macos() { job_zig_cross 'macos'; }
job_ios() { # needs an iOS SDK (Xcode); zig ships no iOS libc headers, so there is nothing faithful to run here.
  command -v xcodebuild >/dev/null || { echo "SKIP: no Xcode/iOS SDK (zig has no iOS libc)"; return 77; }
  cmake -S . -B "$CI_ROOT/ios" -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake -DFORGE_BUILD_TESTS=OFF -DFORGE_BUILD_APPS=OFF \
    -DFORGE_BUILD_EXAMPLES=OFF && cmake --build "$CI_ROOT/ios" --config Release --target forge_c -- -quiet CODE_SIGNING_ALLOWED=NO; }

jobs=${*:-linux-gcc linux-clang linux-tsan linux-asan shells windows-mingw freebsd android macos ios}
rc_all=0
for j in $jobs; do
  fn=job_$(echo "$j" | tr - _); t0=$(date +%s)
  echo "== $j"; $fn > "$CI_OUT/$j.log" 2>&1; r=$?
  case $r in 0) s=PASS;; 77) s=SKIP;; *) s="FAIL($r)"; rc_all=1;; esac
  line="$(date '+%F %T') $j $s $(( $(date +%s) - t0 ))s"; echo "$line"; echo "$line" >> "$CI_OUT/summary.txt"
done
exit $rc_all
