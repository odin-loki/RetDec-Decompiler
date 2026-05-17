
find_package(Threads REQUIRED)

# LLVM may link zlib (e.g. LLVMSupport). Cross MinGW from UNIX: host FindZLIB fails unless
# deps/zlib built retdec-zlib (RETDEC_CROSS_ZLIB_*); otherwise LLVM_ENABLE_ZLIB=OFF.
set(RETDEC_LLVM_WITH_ZLIB ON)
set(_RETDEC_LLVM_USE_CROSS_BUNDLED_ZLIB OFF)
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
	if(RETDEC_CROSS_ZLIB_LIBRARY)
		set(_RETDEC_LLVM_USE_CROSS_BUNDLED_ZLIB ON)
		set(ZLIB_LIBRARIES "${RETDEC_CROSS_ZLIB_LIBRARY}")
		message(STATUS "LLVM: Windows cross — bundled zlib for LLVM (${RETDEC_CROSS_ZLIB_LIBRARY})")
	else()
		set(RETDEC_LLVM_WITH_ZLIB OFF)
		message(STATUS "LLVM: Windows cross — zlib disabled (set RETDEC_BUNDLED_ZLIB_WIN_CROSS=ON or install MinGW zlib).")
	endif()
endif()

if(UNIX OR MINGW)
	if(RETDEC_LLVM_WITH_ZLIB)
		if(NOT _RETDEC_LLVM_USE_CROSS_BUNDLED_ZLIB)
			find_package(ZLIB REQUIRED)
		endif()
	else()
		set(ZLIB_LIBRARIES "")
	endif()
endif()

if(RETDEC_LLVM_WITH_ZLIB)
	set(_LLVM_ZLIB_CMAKE_ARG -DLLVM_ENABLE_ZLIB=ON)
else()
	set(_LLVM_ZLIB_CMAKE_ARG -DLLVM_ENABLE_ZLIB=OFF)
endif()

set(_LLVM_ZLIB_PASSTHROUGH "")
if(_RETDEC_LLVM_USE_CROSS_BUNDLED_ZLIB)
	list(APPEND _LLVM_ZLIB_PASSTHROUGH
		"-DZLIB_INCLUDE_DIR=${RETDEC_CROSS_ZLIB_INCLUDE_DIR}"
		"-DZLIB_LIBRARY=${RETDEC_CROSS_ZLIB_LIBRARY}"
	)
endif()

set(_LLVM_EP_DEPS "")
if(TARGET retdec-zlib)
	list(APPEND _LLVM_EP_DEPS DEPENDS retdec-zlib)
endif()

# Set the default build type to 'Release'
if(NOT CMAKE_BUILD_TYPE)
	set(default_build_type "Release")
	message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
	set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE STRING "Choose the type of build." FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(IS_DEBUG_BUILD "YES")
else()
	set(IS_DEBUG_BUILD "NO")
endif()

if(RETDEC_MSVC_STATIC_RUNTIME)
	set(RT_TYPE_RELEASE "MT")
	set(RT_TYPE_DEBUG "MT")
else()
	set(RT_TYPE_RELEASE "MD")
	set(RT_TYPE_DEBUG "MDd")
endif()

set(LLVM_INSTALL_DIR ${CMAKE_BINARY_DIR}/deps/install/llvm)
set(RETDEC_LLVM_TABLEGEN "" CACHE FILEPATH "Path to host llvm-tblgen for cross builds")

include("${CMAKE_SOURCE_DIR}/cmake/retdec_cross_llvm_tblgen.cmake")

set(LLVM_CROSS_ARGS "")
if(CMAKE_CROSSCOMPILING)
	# Propagate target system identity to the external LLVM project.
	# Without this, LLVM config may infer Linux from host and mix
	# incompatible platform assumptions under MinGW cross builds.
	list(APPEND LLVM_CROSS_ARGS
		-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
		-DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}
	)
	if(CMAKE_RC_COMPILER)
		list(APPEND LLVM_CROSS_ARGS
			-DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}
		)
	endif()
	if(RETDEC_LLVM_TABLEGEN)
		get_filename_component(_tblgen_dir "${RETDEC_LLVM_TABLEGEN}" DIRECTORY)
		list(APPEND LLVM_CROSS_ARGS
			-DLLVM_USE_HOST_TOOLS=ON
			-DLLVM_TABLEGEN=${RETDEC_LLVM_TABLEGEN}
			-DLLVM_NATIVE_TOOL_DIR=${_tblgen_dir}
		)
	endif()
endif()

if(LLVM_LOCAL_DIR)
	message(STATUS "LLVM: using local LLVM directory.")
	set(LLVM_SOURCE_DIR "${LLVM_LOCAL_DIR}")
	set(_LLVM_URL "")
else()
	message(STATUS "LLVM: using remote LLVM revision.")
	set(LLVM_SOURCE_DIR "")
	set(_LLVM_URL "${LLVM_URL}")
endif()

ExternalProject_Add(llvm-project
	SOURCE_DIR ${LLVM_SOURCE_DIR}
	URL ${_LLVM_URL}
	URL_HASH SHA256=${LLVM_ARCHIVE_SHA256}
	DOWNLOAD_NAME llvm.zip
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	${_LLVM_EP_DEPS}
	CMAKE_ARGS
		-DCMAKE_INSTALL_PREFIX=${LLVM_INSTALL_DIR}
		# CMake 4.x rejects cmake_minimum_required < 3.5; LLVM 14 still declares 3.4.3.
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5
		# Match top-level single-config build type (MSVC+Ninja needs consistent /MD vs /MDd).
		-DCMAKE_BUILD_TYPE=${RETDEC_EP_CMAKE_BUILD_TYPE}
		-DCMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}
		-DCMAKE_CXX_FLAGS_DEBUG=${CMAKE_CXX_FLAGS_DEBUG}

		# Force Python3.
		-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}

		# Use provided libtool
		-DCMAKE_LIBTOOL=${CMAKE_LIBTOOL}

		# Targets to be build.
		-DLLVM_TARGETS_TO_BUILD=X86

		# Our tools depending on LLVM require RTTI, so build LLVM with it.
		-DLLVM_REQUIRES_RTTI=YES

		# Debug builds enable assertions; LLVM must be built with matching
		# LLVM_ENABLE_ASSERTIONS to avoid link errors in middle/back-end tools.
		-DLLVM_ENABLE_ASSERTIONS=${IS_DEBUG_BUILD}

		# Disable the emission of warnings, which are useless since we do
		# not modify the LLVM sources (of course, except for a few
		# exceptions).
		-DLLVM_ENABLE_WARNINGS=NO

		# DIA SDK requires ATL (atlbase.h) which is not included in the
		# minimal Build Tools install. We don't need DIA for RetDec.
		-DLLVM_ENABLE_DIA_SDK=OFF

		# Enable exceptions. We want to get std::bad_alloc().
		# Without this, LLVM aborts instead of throwing.
		-DLLVM_ENABLE_RTTI=ON
		-DLLVM_ENABLE_EH=ON

		# Disable the generation of targets for tests (we don't need them).
		-DLLVM_INCLUDE_TOOLS=OFF
		-DLLVM_INCLUDE_UTILS=OFF
		-DLLVM_INCLUDE_RUNTIMES=OFF
		-DLLVM_INCLUDE_EXAMPLES=OFF
		-DLLVM_INCLUDE_TESTS=OFF
		-DLLVM_INCLUDE_GO_TESTS=OFF
		-DLLVM_INCLUDE_BENCHMARKS=OFF
		-DLLVM_INCLUDE_DOCS=OFF

		# Disable build of unnecessary LLVM parts.
		-DLLVM_BUILD_TOOLS=OFF
		-DLLVM_TOOL_LTO_BUILD=OFF
		-DLLVM_BUILD_UTILS=OFF
		-DLLVM_BUILD_RUNTIMES=OFF
		-DLLVM_BUILD_RUNTIME=OFF
		-DLLVM_BUILD_EXAMPLES=OFF
		-DLLVM_BUILD_TESTS=OFF
		-DLLVM_BUILD_BENCHMARKS=OFF
		-DLLVM_BUILD_DOCS=OFF
		-DLLVM_ENABLE_BINDINGS=OFF
		# We don't want this so that we don't have to link with tinfo.
		# It looks like terminal colors are working even without this.
		-DLLVM_ENABLE_TERMINFO=OFF

		# Force the use of the same compiler as used to build the top-level
		# project. Otherwise, the external project may pick up a different
		# compiler, which may result in link errors.
		"${CMAKE_C_COMPILER_OPTION}"
		"${CMAKE_CXX_COMPILER_OPTION}"
		${LLVM_CROSS_ARGS}
		-DCMAKE_POSITION_INDEPENDENT_CODE=${CMAKE_POSITION_INDEPENDENT_CODE}
		${_LLVM_ZLIB_CMAKE_ARG}
		${_LLVM_ZLIB_PASSTHROUGH}
	BUILD_BYPRODUCTS
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMDebugInfoDWARF${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMBitWriter${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMIRReader${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMObject${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMBinaryFormat${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMInstCombine${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMSupport${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMDemangle${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMipo${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMAsmParser${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMBitReader${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMMCParser${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMCodeGen${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMScalarOpts${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMTransformUtils${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMAnalysis${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMTarget${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMCore${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMMC${CMAKE_STATIC_LIBRARY_SUFFIX}
		${LLVM_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVMPasses${CMAKE_STATIC_LIBRARY_SUFFIX}
)
if(LLVM_LOCAL_DIR)
	force_configure_step(llvm-project)
endif()

check_if_variable_changed(LLVM_LOCAL_DIR CHANGED)
if(CHANGED)
	ExternalProject_Get_Property(llvm-project binary_dir)
	message(STATUS "LLVM: path to LLVM directory changed -> cleaning CMake files in ${binary_dir}.")
	clean_cmake_files(${binary_dir})
endif()

set(LLVM_LIB_LIST
	LLVMDebugInfoDWARF
	LLVMBitWriter
	LLVMIRReader
	LLVMObject
	LLVMBinaryFormat
	LLVMInstCombine
	LLVMSupport
	LLVMDemangle
	LLVMipo
	LLVMAsmParser
	LLVMBitReader
	LLVMMCParser
	LLVMCodeGen
	LLVMScalarOpts
	LLVMTransformUtils
	LLVMAnalysis
	LLVMTarget
	LLVMCore
	LLVMMC
	LLVMObject
	LLVMPasses
)

ExternalProject_Get_Property(llvm-project source_dir)

# Create target.

add_library(llvm INTERFACE)
add_library(retdec::deps::llvm ALIAS llvm)
add_dependencies(llvm llvm-project)

target_include_directories(llvm
	SYSTEM INTERFACE
		$<BUILD_INTERFACE:${source_dir}/include>
		$<BUILD_INTERFACE:${LLVM_INSTALL_DIR}/include>
		$<INSTALL_INTERFACE:${RETDEC_INSTALL_DEPS_INCLUDE_DIR}>
)

foreach(LLVM_LIB ${LLVM_LIB_LIST})
	set(LLVM_LIB_NAME  ${CMAKE_STATIC_LIBRARY_PREFIX}${LLVM_LIB}${CMAKE_STATIC_LIBRARY_SUFFIX})
	set(LLVM_LIB_PNAME ${CMAKE_STATIC_LIBRARY_PREFIX}retdec-${LLVM_LIB}${CMAKE_STATIC_LIBRARY_SUFFIX})
	set(LLVM_LIB       ${LLVM_INSTALL_DIR}/lib/${LLVM_LIB_NAME})

	target_link_libraries(llvm INTERFACE
		$<BUILD_INTERFACE:${LLVM_LIB}>
	)

	# Install libs.
	install(
		FILES       ${LLVM_LIB}
		DESTINATION ${RETDEC_INSTALL_LIB_DIR}
		RENAME      ${LLVM_LIB_PNAME}
		OPTIONAL
	)

	list(APPEND LLVM_LIBS_PATHS "${RETDEC_INSTALL_LIB_DIR_ABS}/${LLVM_LIB_PNAME}")
endforeach(LLVM_LIB)

target_link_libraries(llvm
	INTERFACE
		$<INSTALL_INTERFACE:retdec::deps::llvm-libs>
		Threads::Threads
)

if(UNIX)
	set(EXECINFO "")
	if (${CMAKE_SYSTEM_NAME} MATCHES "BSD")
		set(EXECINFO "execinfo")
	endif()
	if(RETDEC_LLVM_WITH_ZLIB)
		target_link_libraries(llvm INTERFACE ${ZLIB_LIBRARIES})
	endif()
	target_link_libraries(llvm
		INTERFACE
			${CMAKE_DL_LIBS}
			${EXECINFO}
	)
elseif(MINGW)
	if(RETDEC_LLVM_WITH_ZLIB)
		target_link_libraries(llvm INTERFACE ${ZLIB_LIBRARIES})
	endif()
endif()

# Install includes.
install(
	DIRECTORY
		${LLVM_INSTALL_DIR}/include/
	DESTINATION
		${RETDEC_INSTALL_DEPS_INCLUDE_DIR}
)

# Install targets.
install(TARGETS llvm
	EXPORT llvm-targets
)

# Export targets.
install(EXPORT llvm-targets
	FILE "retdec-llvm-targets.cmake"
	NAMESPACE retdec::deps::
	DESTINATION ${RETDEC_INSTALL_CMAKE_DIR}
)

# Configure config file.
configure_package_config_file(
	"retdec-llvm-config.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/retdec-llvm-config.cmake"
	INSTALL_DESTINATION ${RETDEC_INSTALL_CMAKE_DIR}
	PATH_VARS
		LLVM_LIBS_PATHS
)

# Install CMake files.
install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}/retdec-llvm-config.cmake"
	DESTINATION
		"${RETDEC_INSTALL_CMAKE_DIR}"
)
