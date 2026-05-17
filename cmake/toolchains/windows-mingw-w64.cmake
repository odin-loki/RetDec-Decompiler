# windows-mingw-w64.cmake — Linux/WSL host → Windows PE cross-compilation
#
# Produces native Windows x86-64 PE binaries using MinGW-w64 (GCC toolchain).
#
# Prerequisites (Ubuntu/Debian WSL):
#   sudo apt install mingw-w64 g++-mingw-w64-x86-64 ninja-build
#
# Usage:
#   cmake -S cmake/superbuild --preset superbuild-windows-cross-mingw
# or manually:
#   cmake -S cmake/superbuild -B build/win-cross \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-mingw-w64.cmake \
#         -DCMAKE_BUILD_TYPE=Release

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ─── Compiler ─────────────────────────────────────────────────────────────────
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc   CACHE FILEPATH "C compiler"   FORCE)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++   CACHE FILEPATH "CXX compiler" FORCE)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres CACHE FILEPATH "RC compiler" FORCE)
set(CMAKE_AR           x86_64-w64-mingw32-ar     CACHE FILEPATH "Archiver"     FORCE)
set(CMAKE_RANLIB       x86_64-w64-mingw32-ranlib  CACHE FILEPATH "Ranlib"      FORCE)
set(CMAKE_STRIP        x86_64-w64-mingw32-strip   CACHE FILEPATH "Strip"       FORCE)

# Keep CMake from trying to run target binaries during try_compile.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING "" FORCE)

# ─── Language standard ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard")
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "")
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "")

# ─── LTO ──────────────────────────────────────────────────────────────────────
# MinGW-w64 supports GCC's -flto; use plugin-based LTO so linker can see IR.
if(NOT DEFINED _RETDEC_MINGW_LTO_SET)
    set(CMAKE_C_COMPILE_OPTIONS_IPO   "-flto" CACHE STRING "" FORCE)
    set(CMAKE_CXX_COMPILE_OPTIONS_IPO "-flto" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_IPO    "-flto" CACHE STRING "" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_IPO "-flto" CACHE STRING "" FORCE)
    set(_RETDEC_MINGW_LTO_SET TRUE CACHE INTERNAL "")
endif()

# ─── sysroot / find-root ──────────────────────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH "/usr/x86_64-w64-mingw32" CACHE PATH "MinGW sysroot")

# User-overrideable path for a pre-built Qt6 Windows cross-build tree.
# Install one via: apt install qt6-base-dev (MinGW variant) or build Qt6 from source
# targeting x86_64-w64-mingw32, then point this variable at the install prefix.
set(RETDEC_QT6_WIN_ROOT
    "$ENV{RETDEC_QT6_WIN_ROOT}"
    CACHE PATH "Path to a Qt6 Windows cross-build (set in env or pass -DRETDEC_QT6_WIN_ROOT=...)")

if(EXISTS "${RETDEC_QT6_WIN_ROOT}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${RETDEC_QT6_WIN_ROOT}")
    list(APPEND CMAKE_PREFIX_PATH    "${RETDEC_QT6_WIN_ROOT}")
    set(Qt6_DIR "${RETDEC_QT6_WIN_ROOT}/lib/cmake/Qt6" CACHE PATH "Qt6 CMake dir")
    message(STATUS "MinGW cross: Qt6 root = ${RETDEC_QT6_WIN_ROOT}")
endif()

# Optional: OpenSSL cross-build root.
set(RETDEC_MINGW_OPENSSL_ROOT
    "$ENV{RETDEC_MINGW_OPENSSL_ROOT}"
    CACHE PATH "Path to cross-compiled OpenSSL for MinGW")

if(EXISTS "${RETDEC_MINGW_OPENSSL_ROOT}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${RETDEC_MINGW_OPENSSL_ROOT}")
    set(OPENSSL_ROOT_DIR "${RETDEC_MINGW_OPENSSL_ROOT}" CACHE PATH "" FORCE)
endif()

# ─── find_* scoping ───────────────────────────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ─── Release flags ────────────────────────────────────────────────────────────
# Cross-compile: no -march=native (we target a generic Windows x86-64 CPU baseline).
if(NOT DEFINED CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG" CACHE STRING "" FORCE)
endif()

# ─── Windows-specific definitions ─────────────────────────────────────────────
# Suppress MinGW's default definition of old-style POSIX names; prefer W32 APIs.
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
