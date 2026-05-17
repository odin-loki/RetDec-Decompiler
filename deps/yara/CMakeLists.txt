
find_package(Threads REQUIRED)

if(YARA_LOCAL_DIR)
	set(YARA_DIR          ${YARA_LOCAL_DIR})
else()
	set(YARA_DIR          ${CMAKE_CURRENT_BINARY_DIR}/yara/src/yara)
endif()

set(YARA_INCLUDE_DIR  ${YARA_DIR}/libyara/include)
set(YARA_LIBRARY_NAME "libyara")

set(YARA_MAKE_PROGRAM "make" CACHE STRING "A path to make tool which should be used to compile yara")
# Use the same make tool when using Unix makefiles
if(${CMAKE_GENERATOR} STREQUAL "Unix Makefiles")
	set(YARA_MAKE_PROGRAM ${CMAKE_MAKE_PROGRAM})
endif()

if(MSVC)
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		set(PLATFORM "x64")
		set(YARA_LIBRARY_NAME "${YARA_LIBRARY_NAME}64")
		set(YARAC_NAME "yarac64.exe" CACHE STRING "Yarac binary name" FORCE)
	else()
		set(PLATFORM "x86")
		set(YARA_LIBRARY_NAME "${YARA_LIBRARY_NAME}32")
		set(YARAC_NAME "yarac32.exe" CACHE STRING "Yarac binary name" FORCE)
	endif()

	# When using Ninja generator with MSVC, CMAKE_VS_MSBUILD_COMMAND is not set.
	# Locate MSBuild.exe explicitly in that case.
	if(NOT CMAKE_VS_MSBUILD_COMMAND)
		# Search in PATH first (set by vcvars64.bat), then try known VS install locations.
		find_program(_YARA_MSBUILD NAMES MSBuild.exe MSBuild)
		if(NOT _YARA_MSBUILD)
			# Manually check common VS install paths (without $ENV{ProgramFiles(x86)} which
			# has invalid parentheses for CMake variable syntax).
			foreach(_vs_ver 18 17 16)
				foreach(_vs_type BuildTools Community Professional Enterprise)
					set(_ms_path
						"C:/Program Files (x86)/Microsoft Visual Studio/${_vs_ver}/${_vs_type}/MSBuild/Current/Bin/MSBuild.exe"
					)
					if(EXISTS "${_ms_path}")
						set(_YARA_MSBUILD "${_ms_path}")
						break()
					endif()
				endforeach()
				if(_YARA_MSBUILD)
					break()
				endif()
			endforeach()
		endif()
		set(_YARA_MSBUILD_CMD "${_YARA_MSBUILD}")
	else()
		set(_YARA_MSBUILD_CMD "${CMAKE_VS_MSBUILD_COMMAND}")
	endif()

	# Determine platform toolset when not provided by the generator.
	if(NOT CMAKE_VS_PLATFORM_TOOLSET)
		# Default to the most recent toolset known for VS 2026 (v145) / 2022 (v143).
		set(_YARA_TOOLSET "v145")
	else()
		set(_YARA_TOOLSET "${CMAKE_VS_PLATFORM_TOOLSET}")
	endif()

	# Determine Windows SDK version when not provided by the generator.
	if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
		set(_YARA_WIN_SDK "10.0")
	else()
		set(_YARA_WIN_SDK "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
	endif()

	set(MSVC_CONFIG $<IF:$<CONFIG:Debug>,Debug,Release>)
	set(YARA_WIN_DIR       ${YARA_DIR}/windows/vs2015)
	set(YARA_SOLUTION      ${YARA_WIN_DIR}/yara.sln)
	set(YARA_LIBRARY_DIR   ${YARA_WIN_DIR}/libyara/${MSVC_CONFIG})
	set(YARA_CLEAN_COMMAND "${_YARA_MSBUILD_CMD}"
		/t:Clean
		${YARA_SOLUTION}
	)
	set(YARA_BUILD_COMMAND "${_YARA_MSBUILD_CMD}"
		/m:${CPUS}
		/p:Platform=${PLATFORM}
		/p:Configuration=${MSVC_CONFIG}
		/p:PlatformToolset=${_YARA_TOOLSET}
		/p:WindowsTargetPlatformVersion=${_YARA_WIN_SDK}
		${YARA_SOLUTION}
	)
	set(YARAC_PATH         ${YARA_WIN_DIR}/${MSVC_CONFIG}/${YARAC_NAME})
else()
	set(YARA_LIBRARY_DIR ${YARA_DIR}/libyara/.libs)
	set(YARA_CLEAN_COMMAND ${YARA_MAKE_PROGRAM} clean)
	set(YARA_BUILD_COMMAND ${YARA_MAKE_PROGRAM} -j${CPUS})
	set(YARAC_PATH         ${YARA_DIR}/yarac)
endif()

# macOS: point YARA build at the active Xcode SDK sysroot.
if(APPLE)
	execute_process(
		COMMAND xcrun --show-sdk-path
		OUTPUT_VARIABLE MACOS_SDK_PATH
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --sysroot=${MACOS_SDK_PATH}")
endif()

# These are not used on MSVC, but we use it on all platforms to detect if
# build configuration changed - changes in config should be mirrored on both
# MSVC & Linux.
# We also clean for MSVC if patch file changed.
set(YARA_CONFIGURE_ARGS
	--prefix=${YARA_INSTALL_DIR}
	--with-pic
	--enable-dotnet
	--enable-macho
	--disable-shared
	--without-crypto
	MAKE=${YARA_MAKE_PROGRAM}
	CC=${CMAKE_C_COMPILER}
	CFLAGS=${CMAKE_C_FLAGS}
	LDFLAGS=${CMAKE_SHARED_LINKER_FLAGS}
)
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
	# Ensure autoconf selects Windows sources (e.g. proc/windows.c) when
	# cross-compiling from Linux to MinGW.
	list(APPEND YARA_CONFIGURE_ARGS
		--host=x86_64-w64-mingw32
	)
endif()
check_if_variable_changed(YARA_CONFIGURE_ARGS CHANGED)
if(CHANGED)
	message(STATUS "YARA: configuration arguments changed -> rebuilding YARA.")
endif()

check_if_variable_changed(YARA_LOCAL_DIR CHANGED)
if(CHANGED)
	message(STATUS "YARA: path to YARA directory changed -> rebuilding YARA.")
endif()

if(YARA_LOCAL_DIR)
	message(STATUS "YARA: using local YARA directory.")
	set(YARA_SOURCE_DIR "${YARA_LOCAL_DIR}")
	set(_YARA_URL "")
else()
	message(STATUS "YARA: using remote YARA revision.")
	set(YARA_SOURCE_DIR "")
	set(_YARA_URL "${YARA_URL}")
endif()

set(YARA_INSTALL_DIR ${CMAKE_BINARY_DIR}/deps/install/yara)
set(YARA_LIB_NAME  ${YARA_LIBRARY_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX})
set(YARA_LIB_NAMEP libretdec-yara${CMAKE_STATIC_LIBRARY_SUFFIX})
set(YARA_LIB_B     ${YARA_LIBRARY_DIR}/${YARA_LIB_NAME})
set(YARA_LIB       ${YARA_INSTALL_DIR}/lib/${YARA_LIB_NAME})

ExternalProject_Add(yara
	SOURCE_DIR ${YARA_SOURCE_DIR}
	URL ${_YARA_URL}
	URL_HASH SHA256=${YARA_ARCHIVE_SHA256}
	PREFIX "yara"
	DOWNLOAD_NAME yara.zip
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	BUILD_IN_SOURCE 1
	BUILD_ALWAYS ${CHANGED}
	# Patch the Yara sources.
	PATCH_COMMAND
		${CMAKE_COMMAND} -Dyara_path=<SOURCE_DIR> -DRETDEC_MSVC_STATIC_RUNTIME=${RETDEC_MSVC_STATIC_RUNTIME} -P ${CMAKE_CURRENT_SOURCE_DIR}/patch.cmake
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ${YARA_CLEAN_COMMAND}
	COMMAND       ${YARA_BUILD_COMMAND}
	INSTALL_COMMAND ""
	BUILD_BYPRODUCTS
		${YARA_LIB}
)
if(YARA_LOCAL_DIR)
	force_configure_step(yara)
endif()

if(NOT MSVC)
	ExternalProject_Add_Step(yara configure-linux
		WORKING_DIRECTORY ${YARA_DIR}
		DEPENDEES configure
		DEPENDERS build
		COMMAND ./bootstrap.sh
		COMMAND ./configure ${YARA_CONFIGURE_ARGS}
	)
else()
	# Clean if patch file changed.
	ExternalProject_Add_Step(yara configure-msvc
		WORKING_DIRECTORY ${YARA_DIR}
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/patch.cmake
		DEPENDEES configure
		DEPENDERS build
		COMMAND ${YARA_CLEAN_COMMAND}
	)
endif()

# Install the library file manually.
# We need it to get rif of MSVC's Release/Debug, because gen exprs cannot be
# used in BUILD_BYPRODUCTS.
# We could install YARA using YARA_INSTALL_DIR, but it would not work on MSVC.
ExternalProject_Add_Step(yara move-install
	WORKING_DIRECTORY ${YARA_DIR}
	DEPENDEES install
	COMMAND ${CMAKE_COMMAND} -E make_directory ${YARA_INSTALL_DIR}/lib
	COMMAND ${CMAKE_COMMAND} -E copy ${YARA_LIB_B} ${YARA_LIB}
)

# Create target.
add_library(libyara INTERFACE)
add_library(retdec::deps::libyara ALIAS libyara)
add_dependencies(libyara yara)

target_include_directories(libyara
	SYSTEM INTERFACE
		$<BUILD_INTERFACE:${YARA_INCLUDE_DIR}>
		$<INSTALL_INTERFACE:${RETDEC_INSTALL_DEPS_INCLUDE_DIR}>
)

target_link_libraries(libyara
	INTERFACE
		$<BUILD_INTERFACE:${YARA_LIB}>
		$<INSTALL_INTERFACE:retdec::deps::libyara-libs>
		Threads::Threads
)

# Install includes.
install(
	DIRECTORY   ${YARA_INCLUDE_DIR}/
	DESTINATION ${RETDEC_INSTALL_DEPS_INCLUDE_DIR}
)

# Install libs.
install(
	FILES       ${YARA_LIB}
	DESTINATION ${RETDEC_INSTALL_LIB_DIR}
	RENAME      ${YARA_LIB_NAMEP}
)

# Install targets.
install(TARGETS libyara
	EXPORT libyara-targets
)

# Export targets.
install(EXPORT libyara-targets
	FILE "retdec-libyara-targets.cmake"
	NAMESPACE retdec::deps::
	DESTINATION ${RETDEC_INSTALL_CMAKE_DIR}
)

# Configure config file.
set(LIBYARA_LIBRARY "${RETDEC_INSTALL_LIB_DIR_ABS}/${YARA_LIB_NAMEP}")
configure_package_config_file(
	"retdec-libyara-config.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/retdec-libyara-config.cmake"
	INSTALL_DESTINATION ${RETDEC_INSTALL_CMAKE_DIR}
	PATH_VARS
		LIBYARA_LIBRARY
)

# Install CMake files.
install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}/retdec-libyara-config.cmake"
	DESTINATION
		"${RETDEC_INSTALL_CMAKE_DIR}"
)

# Install yarac application - we may need it to compile YARA files.
install(
	PROGRAMS "${YARAC_PATH}"
	DESTINATION ${RETDEC_INSTALL_BIN_DIR}
	RENAME "retdec-yarac${CMAKE_EXECUTABLE_SUFFIX}"
)
