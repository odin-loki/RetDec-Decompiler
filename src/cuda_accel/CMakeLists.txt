
# cuda_accel — parked / unintegrated / not product pipeline (C-CUDA-PIPE withdrawn).
# RETDEC_ENABLE_CUDA_ACCEL defaults OFF.
# Falls back to CPU (std::async / std::thread) when CUDA is not available.

cmake_minimum_required(VERSION 3.18)

# MSVC try_compile CUDA_PATH: see cmake/cuda_msvc_trycompile.cmake (included from top CMakeLists).

# Try to find CUDA; failure is non-fatal — we compile a CPU-only stub.
# Skip CUDA entirely when RETDEC_ENABLE_CUDA_ACCEL=OFF (avoids enable_language(CUDA) on
# Windows when nvcc is on PATH but cannot invoke MSVC cl.exe from CMake's try_compile).
if(RETDEC_ENABLE_CUDA_ACCEL)
    include(CheckLanguage)
    check_language(CUDA)

    if(CMAKE_CUDA_COMPILER)
        get_filename_component(_retdec_nvcc_dir "${CMAKE_CUDA_COMPILER}" DIRECTORY)
        get_filename_component(_retdec_cuda_toolkit "${_retdec_nvcc_dir}" DIRECTORY)
        # Help FindCUDAToolkit before/after enable_language (some CMake versions are picky on Windows).
        if(NOT CUDAToolkit_ROOT)
            set(CUDAToolkit_ROOT "${_retdec_cuda_toolkit}" CACHE PATH "CUDA Toolkit root" FORCE)
        endif()
        enable_language(CUDA)
        find_package(CUDAToolkit QUIET)
        if(NOT CUDAToolkit_FOUND)
            find_package(CUDAToolkit QUIET PATHS "${_retdec_cuda_toolkit}" NO_DEFAULT_PATH)
        endif()
        unset(_retdec_nvcc_dir)
        unset(_retdec_cuda_toolkit)
        if(CUDAToolkit_FOUND)
            set(_retdec_has_cuda ON)
            message(STATUS "[cuda_accel] CUDA ${CUDAToolkit_VERSION} found — parked cuda_accel compiled with toolkit present, not product pipeline (C-CUDA-PIPE withdrawn)")
        else()
            set(_retdec_has_cuda OFF)
            message(STATUS "[cuda_accel] CUDA compiler found but CUDAToolkit missing — CPU path, not product fallback (C-CUDA-PIPE withdrawn)")
        endif()
    else()
        set(_retdec_has_cuda OFF)
        message(STATUS "[cuda_accel] No CUDA compiler — CPU-only build")
    endif()
else()
    set(_retdec_has_cuda OFF)
    message(STATUS "[cuda_accel] RETDEC_ENABLE_CUDA_ACCEL=OFF — CPU-only build")
endif()
# CACHE FORCE: refresh each configure so installing CUDA later is picked up without deleting the cache.
set(RETDEC_HAS_CUDA "${_retdec_has_cuda}" CACHE INTERNAL
	"CUDAToolkit found; CUDA accel tests and GPU code paths" FORCE)
set(RETDEC_HAS_CUDA "${_retdec_has_cuda}")
unset(_retdec_has_cuda)

# Source files.  .cu files are compiled by nvcc when CUDA is available;
# otherwise they are compiled as plain C++ (device code guarded by #ifdef).
set(CUDA_ACCEL_SOURCES
    cuda_context.cpp
    cuda_buffer_pool.cpp
    cuda_profiler.cpp
)
set(CUDA_ACCEL_CU_SOURCES
    cuda_disassembler.cu
    cuda_steensgaard.cu
    cuda_type_inferencer.cu
    cuda_semantic_hasher.cu
    cuda_egraph_simplifier.cu
)

# "otherwise they are compiled as plain C++" is what the comment above has
# always said, and nothing made it true.  CMake only knows how to compile a .cu
# when the CUDA language is enabled; with RETDEC_ENABLE_CUDA_ACCEL=OFF -- the
# default -- it does not enable it, and a source whose extension belongs to no
# enabled language is dropped from the target WITHOUT a warning.  Measured on a
# two-source toy project (a.cpp + b.cu, project(... CXX)): the archive built
# clean and contained a.cpp.o alone.  So the default build of this library held
# three of its eight objects, and every host entry point in the five .cu files
# -- the disassembler, Steensgaard, the type inferencer, the semantic hasher and
# the e-graph simplifier -- was an undefined reference to anything that linked
# it.
#
# LANGUAGE CXX routes them to the C++ compiler; it is not sufficient on its own,
# because GCC and Clang dispatch on the extension too and hand a .cu straight to
# the linker ("linker input file unused because linking not done", and then no
# object to archive).  -x c++ is what actually names the language.  That these
# files do compile as C++ is not an assumption: scripts/standalone_check.sh has
# been compiling all five that way, through CU_AS_CXX_MODULES, on every run.
if(NOT RETDEC_HAS_CUDA)
    set_source_files_properties(${CUDA_ACCEL_CU_SOURCES} PROPERTIES LANGUAGE CXX)
    if(MSVC)
        set_source_files_properties(${CUDA_ACCEL_CU_SOURCES} PROPERTIES COMPILE_OPTIONS "/TP")
    else()
        set_source_files_properties(${CUDA_ACCEL_CU_SOURCES} PROPERTIES COMPILE_OPTIONS "-x;c++")
    endif()
endif()

add_library(retdec-cuda-accel STATIC ${CUDA_ACCEL_SOURCES} ${CUDA_ACCEL_CU_SOURCES})
add_library(retdec::cuda-accel ALIAS retdec-cuda-accel)

# Mixed CXX + CUDA: CXX_STANDARD 23 makes CMake require CUDA23 for .cu, which nvcc does not
# support yet. Use C++20 for this target and CUDA 17 for device code.
set_target_properties(retdec-cuda-accel PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    OUTPUT_NAME "retdec-cuda-accel"
)

target_include_directories(retdec-cuda-accel
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if(RETDEC_HAS_CUDA)
    target_compile_definitions(retdec-cuda-accel PUBLIC RETDEC_HAS_CUDA)
    target_link_libraries(retdec-cuda-accel PUBLIC CUDA::cudart)
    set_target_properties(retdec-cuda-accel PROPERTIES
        # Use sm_86 for RTX 3090; adjust or let CMake auto-detect
        CUDA_ARCHITECTURES "86"
    )
endif()

install(
    DIRECTORY ${RETDEC_INCLUDE_DIR}/retdec/cuda_accel
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec
)

install(TARGETS retdec-cuda-accel
    EXPORT retdec-targets
    ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
    LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)
