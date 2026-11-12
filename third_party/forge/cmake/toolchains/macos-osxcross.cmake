# macOS cross build with osxcross (https://github.com/tpoechtrager/osxcross). OSXCROSS_ROOT = the osxcross target dir.
#   cmake -S . -B build/macos -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-osxcross.cmake -DOSXCROSS_HOST=arm64-apple-darwin23
# Native macOS needs no toolchain file: cmake -S . -B build -G Ninja (Metal + NEON are picked up automatically).
set(CMAKE_SYSTEM_NAME Darwin)
if(NOT OSXCROSS_HOST)
  set(OSXCROSS_HOST arm64-apple-darwin23)
endif()
string(REGEX MATCH "^[^-]+" CMAKE_SYSTEM_PROCESSOR ${OSXCROSS_HOST})
set(CMAKE_C_COMPILER $ENV{OSXCROSS_ROOT}/bin/${OSXCROSS_HOST}-clang)
set(CMAKE_CXX_COMPILER $ENV{OSXCROSS_ROOT}/bin/${OSXCROSS_HOST}-clang++)
set(CMAKE_OSX_SYSROOT $ENV{OSXCROSS_ROOT}/SDK/MacOSX.sdk)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0)
set(CMAKE_FIND_ROOT_PATH ${CMAKE_OSX_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
