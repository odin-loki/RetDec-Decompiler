
include(ExternalProject)

include(ProcessorCount)
ProcessorCount(CPUS)

# Drop stale bundled-zlib paths unless this run configures retdec-zlib for LLVM cross.
if(NOT (RETDEC_ENABLE_LLVM AND CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows" AND RETDEC_BUNDLED_ZLIB_WIN_CROSS))
	unset(RETDEC_CROSS_ZLIB_LIBRARY CACHE)
	unset(RETDEC_CROSS_ZLIB_INCLUDE_DIR CACHE)
endif()

if(CMAKE_C_COMPILER)
	set(CMAKE_C_COMPILER_OPTION "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_CXX_COMPILER)
	set(CMAKE_CXX_COMPILER_OPTION "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()

# Ninja/Makefiles: propagate Debug/Release into ExternalProject CMake subbuilds so MSVC CRT and
# _ITERATOR_DEBUG_LEVEL match the main tree (LNK2038). Multi-config generators leave CMAKE_BUILD_TYPE empty.
if(CMAKE_BUILD_TYPE)
	set(RETDEC_EP_CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE INTERNAL "" FORCE)
else()
	set(RETDEC_EP_CMAKE_BUILD_TYPE "Release" CACHE INTERNAL "" FORCE)
endif()

set(MSVC_GE $<BOOL:${MSVC}>)
set(MSVC_CONFIG $<${MSVC_GE}:$<CONFIG>/>)

cond_add_subdirectory(stb RETDEC_ENABLE_STB)
# Linux/Unix -> Windows PE: require in-tree OpenSSL when fileformat (crypto/authenticode) is enabled.
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows" AND RETDEC_ENABLE_FILEFORMAT)
	if(NOT RETDEC_BUNDLED_OPENSSL)
		message(FATAL_ERROR
			"RetDec: cross-compiling for Windows with fileformat requires RETDEC_BUNDLED_OPENSSL=ON. "
			"OpenSSL sources are downloaded under ${CMAKE_BINARY_DIR}/external/openssl-ep and the static "
			"library is installed to ${CMAKE_BINARY_DIR}/external/openssl-install. Remove -DRETDEC_BUNDLED_OPENSSL=OFF.")
	endif()
endif()
if(RETDEC_BUNDLED_OPENSSL AND RETDEC_ENABLE_FILEFORMAT)
	add_subdirectory(openssl)
endif()
cond_add_subdirectory(authenticode-parser RETDEC_ENABLE_AUTHENTICODE_PARSER)
cond_add_subdirectory(capstone RETDEC_ENABLE_CAPSTONE)
cond_add_subdirectory(elfio RETDEC_ENABLE_ELFIO)
cond_add_subdirectory(googletest RETDEC_ENABLE_GOOGLETEST)
cond_add_subdirectory(keystone RETDEC_ENABLE_KEYSTONE)
# zlib arrives one of two ways, and both end at retdec::deps::zlib.
#
# Wherever find_package finds one -- every Unix, macOS, MinGW with the package
# installed -- that is the one. Where it does not, the bundled build in
# deps/zlib runs. Native Windows is the case that has none: find_package there
# reports
#
#     -- Could NOT find ZLIB (missing: ZLIB_LIBRARY ZLIB_INCLUDE_DIR)
#
# and retdec-jvm-parser and retdec-dex-parser, which inflate JAR and APK
# entries, used to answer that by linking the bare Unix name `z`. MSVC cannot
# resolve it and it carries no include directory, so the build died on
# `Cannot open include file: 'zlib.h'` and had done since 2026-08-25.
#
# The cross case keeps its own switch because LLVM wants paths rather than a
# target, and RETDEC_BUNDLED_ZLIB_WIN_CROSS is how that is turned off.
set(_RETDEC_NEED_BUNDLED_ZLIB OFF)
if(RETDEC_ENABLE_LLVM AND CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows" AND RETDEC_BUNDLED_ZLIB_WIN_CROSS)
	set(_RETDEC_NEED_BUNDLED_ZLIB ON)
else()
	find_package(ZLIB QUIET)
	if(NOT ZLIB_FOUND)
		set(_RETDEC_NEED_BUNDLED_ZLIB ON)
	endif()
endif()

if(_RETDEC_NEED_BUNDLED_ZLIB)
	add_subdirectory(zlib)
else()
	message(STATUS "zlib: using the system zlib (${ZLIB_LIBRARIES}).")
	add_library(retdec-deps-zlib INTERFACE)
	add_library(retdec::deps::zlib ALIAS retdec-deps-zlib)
	target_link_libraries(retdec-deps-zlib INTERFACE ZLIB::ZLIB)
endif()
cond_add_subdirectory(llvm RETDEC_ENABLE_LLVM)
cond_add_subdirectory(rapidjson RETDEC_ENABLE_RAPIDJSON)
cond_add_subdirectory(tinyxml2 RETDEC_ENABLE_TINYXML2)
cond_add_subdirectory(yara RETDEC_ENABLE_YARA)
cond_add_subdirectory(yaramod RETDEC_ENABLE_YARAMOD)
cond_add_subdirectory(tlsh RETDEC_ENABLE_TLSH)
if(RETDEC_ENABLE_NEURAL AND RETDEC_ENABLE_LLAMACPP)
	add_subdirectory(llamacpp)
endif()
if(RETDEC_ENABLE_XSIMD)
	add_subdirectory(xsimd)
endif()
