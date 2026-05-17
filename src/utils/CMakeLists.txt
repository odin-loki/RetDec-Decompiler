
add_library(utils STATIC
	io/log.cpp
	io/logger.cpp
	alignment.cpp
	byte_value_storage.cpp
	binary_path.cpp
	conversion.cpp
	crc32.cpp
	dynamic_buffer.cpp
	file_io.cpp
	math.cpp
	memory.cpp
	ord_lookup.cpp
	string.cpp
	system.cpp
	time.cpp
	version.cpp
	${RETDEC_DEPS_DIR}/whereami/whereami/whereami.c
)
add_library(retdec::utils ALIAS utils)

target_compile_features(utils PUBLIC cxx_std_17)

target_compile_definitions(utils PRIVATE
	RETDEC_GIT_COMMIT_HASH="${RETDEC_GIT_COMMIT_HASH}"
	RETDEC_BUILD_DATE="${RETDEC_BUILD_DATE}"
	RETDEC_GIT_VERSION_TAG="${RETDEC_GIT_VERSION_TAG}"
)

target_include_directories(utils
	PUBLIC
		$<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
		$<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
	PRIVATE
		$<BUILD_INTERFACE:${RETDEC_DEPS_DIR}/whereami>
)

# We may need to link filesystem library manually. This is only relevant for
# GCC/Clang on Linux — MSVC's std::filesystem ships in the standard runtime
# and `stdc++fs` is meaningless (and missing) on Windows. Don't even probe
# for it on MSVC so a stale/leftover find_library cache value can't poison
# the link line for every consuming target.
if(NOT MSVC AND NOT APPLE)
	find_library(STD_CPP_FS stdc++fs)
	if(STD_CPP_FS)
		message(STATUS "-- Library stdc++fs found -> Linking with ${STD_CPP_FS}")
		target_link_libraries(utils PRIVATE stdc++fs)
	elseif(UNIX AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		message(STATUS "-- Library stdc++fs NOT found & Linux+GCC -> Linking with stdc++fs")
		target_link_libraries(utils PRIVATE stdc++fs)
	else()
		message(STATUS "-- Library stdc++fs NOT found -> linking utils without stdc++fs library")
	endif()
else()
	# Explicitly clear any cached value so a previous bad probe can't leak in.
	unset(STD_CPP_FS CACHE)
endif()

# Disable the min() and max() macros to prevent errors when using e.g.
# std::numeric_limits<...>::max()
# (http://stackoverflow.com/questions/1904635/warning-c4003-and-errors-c2589-and-c2059-on-x-stdnumeric-limitsintmax).
# Any target that includes "windows.h" needs to define this.
if(MSVC)
	target_compile_definitions(utils PUBLIC NOMINMAX)
endif()

set_target_properties(utils
	PROPERTIES
		OUTPUT_NAME "retdec-utils"
)

# Install includes.
install(
	DIRECTORY ${RETDEC_INCLUDE_DIR}/retdec/utils
	DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec
)

# Install libs.
install(TARGETS utils
	EXPORT utils-targets
	ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
	LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)

# Export targets.
install(EXPORT utils-targets
	FILE "retdec-utils-targets.cmake"
	NAMESPACE retdec::
	DESTINATION ${RETDEC_INSTALL_CMAKE_DIR}
)

# Install CMake files.
configure_file(
	"retdec-utils-config.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/retdec-utils-config.cmake"
	@ONLY
)
install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}/retdec-utils-config.cmake"
	DESTINATION
		"${RETDEC_INSTALL_CMAKE_DIR}"
)

# -----------------------------------------------------------------------
# GPU Scanner (CUDA optional, CPU fallback automatic)
# -----------------------------------------------------------------------
option(RETDEC_ENABLE_CUDA "Enable CUDA GPU acceleration (signature matching, entropy)" OFF)
set(
    RETDEC_CUDA_ARCHITECTURES
    "60;70;75;80;86;89"
    CACHE STRING
    "CUDA architectures for retdec-gpu-scanner (semicolon-separated, e.g. 30;35;50 or 60;75;86)"
)

if(RETDEC_ENABLE_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        find_package(CUDAToolkit QUIET)
        set(RETDEC_CUDA_FOUND TRUE)
        message(STATUS "RetDec: CUDA enabled — compiler: ${CMAKE_CUDA_COMPILER}")
    else()
        message(WARNING "RetDec: CUDA requested but no compiler found — using CPU fallback")
        set(RETDEC_CUDA_FOUND FALSE)
    endif()
else()
    set(RETDEC_CUDA_FOUND FALSE)
endif()

if(RETDEC_CUDA_FOUND)
    add_library(retdec-gpu-scanner STATIC gpu_scanner.cu)
    set_target_properties(retdec-gpu-scanner PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        # Default targets: Pascal(60), Volta(70), Turing(75), Ampere(80,86), Ada(89).
        # Override via -DRETDEC_CUDA_ARCHITECTURES="..."
        CUDA_ARCHITECTURES "${RETDEC_CUDA_ARCHITECTURES}"
        OUTPUT_NAME "retdec-gpu-scanner"
    )
    target_compile_definitions(retdec-gpu-scanner PUBLIC RETDEC_CUDA_ENABLED)
    target_link_libraries(retdec-gpu-scanner PUBLIC CUDA::cudart)
else()
    add_library(retdec-gpu-scanner STATIC gpu_scanner_cpu.cpp)
    set_target_properties(retdec-gpu-scanner PROPERTIES OUTPUT_NAME "retdec-gpu-scanner")
endif()

target_compile_features(retdec-gpu-scanner PUBLIC cxx_std_17)
target_include_directories(retdec-gpu-scanner
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)
add_library(retdec::gpu-scanner ALIAS retdec-gpu-scanner)

install(TARGETS retdec-gpu-scanner
    EXPORT retdec-targets
    ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
    LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)
