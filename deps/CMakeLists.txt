
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
if(RETDEC_ENABLE_LLVM AND CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows" AND RETDEC_BUNDLED_ZLIB_WIN_CROSS)
	add_subdirectory(zlib)
endif()
cond_add_subdirectory(llvm RETDEC_ENABLE_LLVM)
cond_add_subdirectory(rapidjson RETDEC_ENABLE_RAPIDJSON)
cond_add_subdirectory(tinyxml2 RETDEC_ENABLE_TINYXML2)
cond_add_subdirectory(yara RETDEC_ENABLE_YARA)
cond_add_subdirectory(yaramod RETDEC_ENABLE_YARAMOD)
cond_add_subdirectory(tlsh RETDEC_ENABLE_TLSH)
