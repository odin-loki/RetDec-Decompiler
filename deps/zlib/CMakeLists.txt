# Bundled zlib.
#
# Two consumers, and they want different things from it:
#
#   * LLVM, when cross-compiling Linux -> Windows (MinGW). It needs
#     ZLIB_INCLUDE_DIR and ZLIB_LIBRARY as PATHS, which RETDEC_CROSS_ZLIB_*
#     carry over to deps/llvm.
#   * retdec-jvm-parser and retdec-dex-parser, which inflate JAR and APK
#     entries. They need a TARGET.
#
# The second consumer is why this is no longer cross-only. Both of those
# libraries linked the bare Unix name `z`, which MSVC cannot resolve and which
# carries no include directory with it, so every native Windows build since
# 2026-08-25 died on the first of them:
#
#     src\jvm_parser\jvm_jar_reader.cpp(26): fatal error C1083:
#         Cannot open include file: 'zlib.h': No such file or directory
#
# deps/CMakeLists.txt decides whether this directory is entered at all. It is
# skipped wherever find_package(ZLIB) succeeds, which is every Unix and macOS,
# so this builds only where the platform has no zlib to find.

include(ExternalProject)

set(ZLIB_INSTALL_DIR "${EP_PREFIX}/zlib-install")
file(MAKE_DIRECTORY "${ZLIB_INSTALL_DIR}/include")

# zlib 1.3.2 rewrote its CMake: the static target is still `zlibstatic`, but
# OUTPUT_NAME is `z` on Unix and `zs` on WIN32. CMAKE_DEBUG_POSTFIX is `d` on
# WIN32, so Debug installs zsd.lib / libzsd.a. Unix Debug stays libz.a.
set(_zlib_output_name "z")
if(WIN32)
	set(_zlib_output_name "zs")
endif()
set(_zlib_postfix "")
if(WIN32 AND RETDEC_EP_CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(_zlib_postfix "d")
endif()
set(RETDEC_ZLIB_STATIC_LIB
	"${ZLIB_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${_zlib_output_name}${_zlib_postfix}${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(_RETDEC_ZLIB_EP "${CMAKE_BINARY_DIR}/external/zlib-ep")

set(_ZLIB_CMAKE_EXTRA "")
if(CMAKE_CROSSCOMPILING)
	list(APPEND _ZLIB_CMAKE_EXTRA "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
	list(APPEND _ZLIB_CMAKE_EXTRA "-DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(CMAKE_RC_COMPILER)
	list(APPEND _ZLIB_CMAKE_EXTRA "-DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}")
endif()

if(MSVC)
	# Two things MSVC needs and the MinGW path never did.
	#
	# The runtime has to be the one the rest of the tree uses or the final link
	# fails with LNK2038 on RuntimeLibrary. CMAKE_MSVC_RUNTIME_LIBRARY is only
	# honoured under CMP0091 NEW, and zlib's own cmake_minimum_required puts it
	# at OLD, so the policy default goes across too.
	#
	# The debug postfix is NOT handled here: zlib sets it on its own targets and
	# wins, so the file name above accounts for it instead.
	if(RETDEC_EP_CMAKE_BUILD_TYPE STREQUAL "Debug")
		set(_zlib_rt_debug "Debug")
	else()
		set(_zlib_rt_debug "")
	endif()
	if(RETDEC_MSVC_STATIC_RUNTIME)
		set(_zlib_rt "MultiThreaded${_zlib_rt_debug}")
	else()
		set(_zlib_rt "MultiThreaded${_zlib_rt_debug}DLL")
	endif()
	list(APPEND _ZLIB_CMAKE_EXTRA
		"-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
		"-DCMAKE_MSVC_RUNTIME_LIBRARY=${_zlib_rt}"
	)
endif()

# ccache/sccache, when this build has one. See retdec_compiler_launcher_args.
retdec_compiler_launcher_args(_ZLIB_LAUNCHER_ARGS)

ExternalProject_Add(retdec-zlib
	PREFIX "${_RETDEC_ZLIB_EP}"
	# Unquoted on purpose: ZLIB_URL is a ;-separated LIST of mirrors and
	# ExternalProject tries each in turn. Quoting it would hand over one
	# argument with a semicolon in it.
	URL ${ZLIB_URL}
	URL_HASH "SHA256=${ZLIB_ARCHIVE_SHA256}"
	DOWNLOAD_NAME zlib-1.3.2.tar.gz
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	CMAKE_ARGS
		-DCMAKE_INSTALL_PREFIX=${ZLIB_INSTALL_DIR}
		${_ZLIB_LAUNCHER_ARGS}
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5
		-DCMAKE_INSTALL_LIBDIR=lib
		-DCMAKE_BUILD_TYPE=${RETDEC_EP_CMAKE_BUILD_TYPE}
		-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
		"${CMAKE_C_COMPILER_OPTION}"
		-DZLIB_BUILD_SHARED=OFF
		-DZLIB_BUILD_STATIC=ON
		-DZLIB_BUILD_TESTING=OFF
		-DCMAKE_POSITION_INDEPENDENT_CODE=${CMAKE_POSITION_INDEPENDENT_CODE}
		-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
		${_ZLIB_CMAKE_EXTRA}
	BUILD_BYPRODUCTS "${RETDEC_ZLIB_STATIC_LIB}"
)

# The paths deps/llvm reads, set only where it reads them. Setting them on a
# native Windows build would hand LLVM a cross-build's answer.
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
	set(RETDEC_CROSS_ZLIB_LIBRARY "${RETDEC_ZLIB_STATIC_LIB}" CACHE INTERNAL "Bundled zlib library path (cross Windows)")
	set(RETDEC_CROSS_ZLIB_INCLUDE_DIR "${ZLIB_INSTALL_DIR}/include" CACHE INTERNAL "Bundled zlib headers (cross Windows)")
endif()

# The target the parsers link. Same shape as retdec::deps::capstone.
add_library(retdec-deps-zlib INTERFACE)
add_library(retdec::deps::zlib ALIAS retdec-deps-zlib)
add_dependencies(retdec-deps-zlib retdec-zlib)

target_include_directories(retdec-deps-zlib
	SYSTEM INTERFACE
		$<BUILD_INTERFACE:${ZLIB_INSTALL_DIR}/include>
)

target_link_libraries(retdec-deps-zlib INTERFACE
	$<BUILD_INTERFACE:${RETDEC_ZLIB_STATIC_LIB}>
)

message(STATUS "zlib: bundled, installing to ${ZLIB_INSTALL_DIR} (target retdec-zlib)")
