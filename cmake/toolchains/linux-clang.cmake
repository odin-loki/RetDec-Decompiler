# linux-clang.cmake — native Linux/WSL build using Clang
#
# Usage (from repo root or WSL):
#   cmake -S cmake/superbuild --preset superbuild-linux-clang
# or manually:
#   cmake -S cmake/superbuild -B build/linux-clang \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-clang.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# Requirements:
#   apt install clang lld ninja-build

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ─── Compiler selection ───────────────────────────────────────────────────────
# Prefer versioned binaries (clang-17, clang-16 …) to avoid accidental GCC pick-up.
find_program(_CLANG_BIN NAMES clang-18 clang-17 clang-16 clang-15 clang)
find_program(_CLANGXX_BIN NAMES clang++-18 clang++-17 clang++-16 clang++-15 clang++)

if(_CLANG_BIN)
    set(CMAKE_C_COMPILER "${_CLANG_BIN}" CACHE FILEPATH "C compiler" FORCE)
else()
    set(CMAKE_C_COMPILER clang CACHE FILEPATH "C compiler" FORCE)
endif()

if(_CLANGXX_BIN)
    set(CMAKE_CXX_COMPILER "${_CLANGXX_BIN}" CACHE FILEPATH "CXX compiler" FORCE)
else()
    set(CMAKE_CXX_COMPILER clang++ CACHE FILEPATH "CXX compiler" FORCE)
endif()

# ─── Linker ───────────────────────────────────────────────────────────────────
# lld is dramatically faster than ld.bfd and required for LTO with Clang.
find_program(_LLD_BIN NAMES ld.lld-18 ld.lld-17 ld.lld-16 ld.lld)
if(_LLD_BIN)
    set(CMAKE_EXE_LINKER_FLAGS_INIT    "-fuse-ld=lld" CACHE STRING "" FORCE)
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld" CACHE STRING "" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld" CACHE STRING "" FORCE)
endif()

# ─── Language standard ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard")
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "")
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "")

# ─── LTO / IPO ────────────────────────────────────────────────────────────────
# CMAKE_INTERPROCEDURAL_OPTIMIZATION is set at the project / preset level.
# We configure Clang's ThinLTO here so IPO works out of the box.
# ThinLTO gives near-full-LTO quality with much better compile times.
if(NOT DEFINED _RETDEC_LTO_FLAGS_SET)
    set(CMAKE_C_COMPILE_OPTIONS_IPO   "-flto=thin" CACHE STRING "" FORCE)
    set(CMAKE_CXX_COMPILE_OPTIONS_IPO "-flto=thin" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_IPO    "-flto=thin" CACHE STRING "" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_IPO "-flto=thin" CACHE STRING "" FORCE)
    set(_RETDEC_LTO_FLAGS_SET TRUE CACHE INTERNAL "")
endif()

# ─── Position independent code ────────────────────────────────────────────────
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "")

# ─── Release flags ────────────────────────────────────────────────────────────
# Clang-specific: add -march=native for native Release builds (CPU-specific optimisations).
# Override in child projects with -DCMAKE_CXX_FLAGS_RELEASE if cross-compiling.
if(NOT DEFINED CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native" CACHE STRING "" FORCE)
    set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG -march=native" CACHE STRING "" FORCE)
endif()
