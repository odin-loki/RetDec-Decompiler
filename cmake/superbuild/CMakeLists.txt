cmake_minimum_required(VERSION 3.26)

project(retdec-superbuild
    LANGUAGES C CXX
    VERSION 0.1.0
    DESCRIPTION "RetDec Enhanced Decompiler Suite — top-level superbuild"
)

# ─── Standards & policies ─────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

cmake_policy(SET CMP0135 NEW) # URL download timestamps

include(GNUInstallDirs)
include(ExternalProject)
include(CheckIPOSupported)

# ─── LTO (Link-Time Optimisation) ─────────────────────────────────────────────
# Honour CMAKE_INTERPROCEDURAL_OPTIMIZATION when set (e.g. from preset).
# For Release / RelWithDebInfo builds, check if the compiler supports LTO and
# enable it automatically when the caller hasn't already decided.
if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg LANGUAGES CXX)
        if(_ipo_ok)
            message(STATUS "LTO: supported — enabling for ${CMAKE_BUILD_TYPE}")
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        else()
            message(STATUS "LTO: not supported — ${_ipo_msg}")
        endif()
    endif()
endif()

# ─── Paths ────────────────────────────────────────────────────────────────────
set(RETDEC_SUPERBUILD_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")

# Single install tree so all components land in the same prefix.
set(RETDEC_SUPERBUILD_INSTALL_PREFIX
    "${CMAKE_BINARY_DIR}/install"
    CACHE PATH "Install prefix for the whole superbuild (bin/lib/include/share layout)")

set(RETDEC_SUPERBUILD_BUILD_TYPE
    "${CMAKE_BUILD_TYPE}"
    CACHE STRING "CMake build type forwarded to child builds")

# ─── Options ──────────────────────────────────────────────────────────────────
option(RETDEC_SUPERBUILD_ENABLE_RETDEC_CORE "Build RetDec core from this repo"         ON)
option(RETDEC_SUPERBUILD_ENABLE_QT_GUI      "Build Qt6 GUI component"                  ON)
option(RETDEC_SUPERBUILD_ENABLE_CUDA_LAYER "Build CUDA acceleration layer"             ON)
option(RETDEC_SUPERBUILD_ENABLE_ML_FRAMEWORK "Build Qwen3 ML inference framework"      ON)
option(RETDEC_SUPERBUILD_ENABLE_TESTS       "Build unit/integration tests in RetDec core" ON)

# ─── Shared cmake args forwarded to every ExternalProject ─────────────────────
set(_COMMON_CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX:PATH=${RETDEC_SUPERBUILD_INSTALL_PREFIX}
    -DCMAKE_BUILD_TYPE:STRING=${RETDEC_SUPERBUILD_BUILD_TYPE}
    -DCMAKE_CXX_STANDARD:STRING=23
    -DCMAKE_CXX_STANDARD_REQUIRED:BOOL=ON
    -DCMAKE_CXX_EXTENSIONS:BOOL=OFF
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=${CMAKE_INTERPROCEDURAL_OPTIMIZATION}
    # Propagate install-dir layout variables so all components are consistent.
    -DCMAKE_INSTALL_BINDIR:STRING=${CMAKE_INSTALL_BINDIR}
    -DCMAKE_INSTALL_LIBDIR:STRING=${CMAKE_INSTALL_LIBDIR}
    -DCMAKE_INSTALL_INCLUDEDIR:STRING=${CMAKE_INSTALL_INCLUDEDIR}
    -DCMAKE_INSTALL_DATAROOTDIR:STRING=${CMAKE_INSTALL_DATAROOTDIR}
)
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND _COMMON_CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}")
endif()
if(CMAKE_GENERATOR)
    list(APPEND _COMMON_CMAKE_ARGS
        "-DCMAKE_GENERATOR:STRING=${CMAKE_GENERATOR}")
endif()
if(CMAKE_MAKE_PROGRAM)
    list(APPEND _COMMON_CMAKE_ARGS
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}")
endif()

# ─── 1. retdec-core ───────────────────────────────────────────────────────────
# Builds the main RetDec library tree (fileformat, bin2llvmir, llvmir2hll, etc.)
# and produces:  retdec-core (shared lib), retdec-decompiler (CLI executable).
if(RETDEC_SUPERBUILD_ENABLE_RETDEC_CORE)
    ExternalProject_Add(retdec-core
        SOURCE_DIR "${RETDEC_SUPERBUILD_SOURCE_DIR}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/retdec-core-build"
        INSTALL_DIR "${RETDEC_SUPERBUILD_INSTALL_PREFIX}"
        CMAKE_ARGS
            ${_COMMON_CMAKE_ARGS}
            -DRETDEC_TESTS:BOOL=${RETDEC_SUPERBUILD_ENABLE_TESTS}
            # Do not pass individual RETDEC_ENABLE_* here: that turns RETDEC_ENABLE_ALL OFF and
            # leaves retdec-decompiler dependencies (e.g. ar-extractor) disabled — see cmake/options.cmake.
            -DRETDEC_ENABLE_ALL:BOOL=ON
        BUILD_ALWAYS OFF
        STEP_TARGETS build install
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
    )
endif()

# ─── 2. retdec-cuda-accel (CUDA acceleration layer) ──────────────────────────
# Produces: libretdec-cuda-accel (static), headers installed to include/retdec/cuda_accel/.
# Falls back to CPU-only (std::async) when no CUDA compiler is detected.
if(RETDEC_SUPERBUILD_ENABLE_CUDA_LAYER)
    # cuda_accel is built as part of retdec-core — no separate stub needed.
    message(STATUS "[superbuild] CUDA acceleration is built inside retdec-core.")
endif()

# ─── 3. retdec-ml (Qwen3 ML inference framework) ─────────────────────────────
# Produces: libretdec-ml (static), headers installed to include/retdec/ml/.
if(RETDEC_SUPERBUILD_ENABLE_ML_FRAMEWORK)
    add_subdirectory(stubs/retdec-ml)
endif()

# ─── 4. retdec-gui (Qt6 GUI application) ──────────────────────────────────────
# Produces: retdec-gui executable installed to bin/.
# Falls back to a no-op stub when Qt6 is not available.
if(RETDEC_SUPERBUILD_ENABLE_QT_GUI)
    add_subdirectory(stubs/retdec-gui)
endif()

# ─── Aggregate convenience target ─────────────────────────────────────────────
add_custom_target(retdec-superbuild-all
    COMMENT "Building all RetDec superbuild components"
)

foreach(_tgt retdec-core retdec-ml retdec-gui)
    if(TARGET ${_tgt})
        add_dependencies(retdec-superbuild-all ${_tgt})
    endif()
endforeach()

# ─── Install layout summary ────────────────────────────────────────────────────
# All components install into ${RETDEC_SUPERBUILD_INSTALL_PREFIX}:
#   bin/        — retdec-decompiler, retdec-gui, helper tools
#   lib/        — libretdec-*.a / .so, libretdec-cuda-accel.a, libretdec-ml.a
#   include/    — public headers (retdec/, retdec/cuda_accel/, retdec/ml/)
#   share/retdec/ — support data, YARA signatures, patterns, config files
message(STATUS "RetDec superbuild install prefix: ${RETDEC_SUPERBUILD_INSTALL_PREFIX}")
message(STATUS "  bin/       → ${RETDEC_SUPERBUILD_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
message(STATUS "  lib/       → ${RETDEC_SUPERBUILD_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
message(STATUS "  include/   → ${RETDEC_SUPERBUILD_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")
message(STATUS "  share/     → ${RETDEC_SUPERBUILD_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}")
