# Android arm64-v8a with the NDK (NEON + Vulkan). ANDROID_NDK_HOME must point at the NDK (r26+).
#   cmake -S . -B build/android-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-arm64.cmake -DFORGE_JNI=ON
# For x86_64 (emulator) pass -DANDROID_ABI=x86_64.
if(NOT ANDROID_ABI)
  set(ANDROID_ABI arm64-v8a)
endif()
set(ANDROID_PLATFORM android-26 CACHE STRING "")
set(ANDROID_STL c++_shared CACHE STRING "")
include($ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake)
