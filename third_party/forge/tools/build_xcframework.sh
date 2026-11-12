#!/bin/sh
# Builds FORGE.xcframework (iOS device, iOS simulator, macOS) for bindings/apple. Run on macOS with Xcode.
set -eu
cd "$(dirname "$0")/.."
out=bindings/apple/FORGE.xcframework; rm -rf "$out" build/xcf; mkdir -p build/xcf/include
cp inference/include/forge.h build/xcf/include/
build() { # name, extra cmake args
  cmake -S . -B "build/xcf/$1" -G Xcode -DFORGE_BUILD_TESTS=OFF -DFORGE_BUILD_APPS=OFF "$@" >/dev/null
  cmake --build "build/xcf/$1" --config Release --target forge_c
}
build ios -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake
build sim -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake -DFORGE_IOS_SIMULATOR=ON
build mac -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
lib() { find "build/xcf/$1" -name 'libforge*.dylib' -path '*Release*' | head -1; }
xcodebuild -create-xcframework \
  -library "$(lib ios)" -headers build/xcf/include \
  -library "$(lib sim)" -headers build/xcf/include \
  -library "$(lib mac)" -headers build/xcf/include -output "$out"
echo "wrote $out"
