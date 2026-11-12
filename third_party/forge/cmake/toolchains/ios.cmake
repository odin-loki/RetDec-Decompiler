# iOS / iPadOS (device arm64, or simulator with -DFORGE_IOS_SIMULATOR=ON). Needs Xcode (macOS host).
#   cmake -S . -B build/ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake
# or use tools/build_xcframework.sh to produce FORGE.xcframework for the Swift package (bindings/apple).
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.3 CACHE STRING "")   # std::to_chars(float) (std::format) needs iOS 16.3
if(FORGE_IOS_SIMULATOR)
  set(CMAKE_OSX_SYSROOT iphonesimulator)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
else()
  set(CMAKE_OSX_SYSROOT iphoneos)
  set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
  set(CMAKE_SYSTEM_PROCESSOR arm64)   # pick the NEON kernels, not the host's x86 ones
endif()
set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO)
set(FORGE_BUILD_TESTS OFF CACHE BOOL "")   # no executables on device; tests run via XCTest in bindings/apple
set(FORGE_BUILD_APPS OFF CACHE BOOL "")
