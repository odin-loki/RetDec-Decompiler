#!/bin/sh
# Mobile build tests. Exit 77 (ctest SKIP) when the toolchain/device isn't present.
#   mobile_build.sh android <abi>   NDK cross-build of forge_c + forge_jni, checks JNI exports
#   mobile_build.sh android-device  runs the Kotlin/JNI instrumented tests through gradle on an attached device
#   mobile_build.sh ios             builds FORGE.xcframework and runs `swift test` (macOS + Xcode)
set -eu
src=$(cd "$(dirname "$0")/.." && pwd); bin=${FORGE_MOBILE_BUILD_DIR:-${TMPDIR:-/tmp}/forge-mobile}
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${FORGE_NDK:-}}}
case "$1" in
android)
  [ -n "$ndk" ] && [ -f "$ndk/build/cmake/android.toolchain.cmake" ] || { echo "SKIP: no Android NDK (set ANDROID_NDK_HOME)"; exit 77; }
  b="$bin/android-$2"
  cmake -S "$src" -B "$b" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$2" -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release -DFORGE_JNI=ON \
    -DFORGE_BUILD_TESTS=OFF -DFORGE_BUILD_APPS=OFF \
    -DCMAKE_CXX_FLAGS="-fexperimental-library" -DCMAKE_EXE_LINKER_FLAGS="-fexperimental-library" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fexperimental-library" >/dev/null   # NDK libc++: jthread/stop_token/<format> (CMakeLists.txt adds it too)
  cmake --build "$b" --target forge_jni -j "${FORGE_MOBILE_JOBS:-3}"
  nm=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-nm | head -1)
  n=$("$nm" -D --defined-only "$b/libforge_jni.so" | grep -c ' T Java_dev_forge_ForgeModel_native')
  want=$(grep -c '^JNIEXPORT' "$src/bindings/android/jni/forge_jni.c")
  echo "JNI exports: $n / $want"; [ "$n" -eq "$want" ]
  "$nm" -D --defined-only "$b/libforge.so" | grep -q ' T forge_open$';;
android-device)
  command -v adb >/dev/null && [ -n "$(adb devices | sed 1d | grep -w device || true)" ] || { echo "SKIP: no adb device"; exit 77; }
  command -v gradle >/dev/null || { echo "SKIP: no gradle"; exit 77; }
  cd "$src/bindings/android" && gradle :forge:connectedAndroidTest;;
ios)
  command -v xcodebuild >/dev/null && command -v swift >/dev/null || { echo "SKIP: needs macOS with Xcode"; exit 77; }
  "$src/tools/build_xcframework.sh" && cd "$src/bindings/apple" && swift test;;
*) echo "usage: $0 android <abi> | android-device | ios"; exit 2;;
esac
