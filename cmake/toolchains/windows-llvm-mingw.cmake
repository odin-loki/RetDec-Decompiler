# Unix host -> Windows PE via llvm-mingw (Clang); not MSVC.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# User-space llvm-mingw installation root.
set(LLVM_MINGW_ROOT "$ENV{HOME}/toolchains/llvm-mingw" CACHE PATH "Path to llvm-mingw root")

set(CMAKE_C_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")

set(CMAKE_AR "${LLVM_MINGW_ROOT}/bin/llvm-ar")
set(CMAKE_RANLIB "${LLVM_MINGW_ROOT}/bin/llvm-ranlib")
set(CMAKE_NM "${LLVM_MINGW_ROOT}/bin/llvm-nm")

# Do not try to run target executables during configure checks.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
