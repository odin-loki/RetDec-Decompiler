# Bundled OpenSSL (static): official tarball under ${CMAKE_BINARY_DIR}/external/openssl-ep/src,
# install prefix ${EP_PREFIX}/openssl-install.
#
# Supported platforms:
#   Linux (native)                     → Configure linux-x86_64 + make
#   Windows via MinGW cross (Linux→Win) → Configure mingw64 + make
#   Windows via MinGW native (MSYS2)    → Configure mingw64 + make
#   Windows via MSVC (native)           → Configure VC-WIN64A + nmake
#
# MSVC path requires: Perl, NASM (optional — no-asm disables it), and a
# Developer Command Prompt environment (cl.exe on PATH) — satisfied
# automatically when CMake is invoked from the VS Developer PowerShell or
# via cmake --build with the Visual Studio generator, or when Ninja is used
# from the Developer Command Prompt.

if(NOT PERL_EXECUTABLE)
	find_program(PERL_EXECUTABLE NAMES perl perl.exe REQUIRED)
endif()

include(ExternalProject)
include(ProcessorCount)
ProcessorCount(CPUS)
if(NOT CPUS GREATER 0)
	set(CPUS 4)
endif()

set(RETDEC_OPENSSL_VERSION "3.2.6")
set(RETDEC_OSSL_TARBALL "openssl-${RETDEC_OPENSSL_VERSION}.tar.gz")
set(RETDEC_OSSL_URL "https://github.com/openssl/openssl/releases/download/openssl-${RETDEC_OPENSSL_VERSION}/${RETDEC_OSSL_TARBALL}")
# Official GitHub release SHA256 for OpenSSL 3.2.6.
set(RETDEC_OSSL_SHA256 "89681a9ddaa9ed7cf25ea8ef61338db805200bae47d00510490623547380c148")

set(RETDEC_OPENSSL_INSTALL_DIR "${EP_PREFIX}/openssl-install")
# CMake 3.24+ may reject INTERFACE_INCLUDE_DIRECTORIES that do not exist yet;
# ExternalProject populates this directory during the build.
file(MAKE_DIRECTORY "${RETDEC_OPENSSL_INSTALL_DIR}/include")
file(TO_CMAKE_PATH "${RETDEC_OPENSSL_INSTALL_DIR}" _ossl_inst)
string(REPLACE "\\" "/" RETDEC_OSSL_PREFIX "${_ossl_inst}")

set(_RETDEC_OSSL_EP_PREFIX "${CMAKE_BINARY_DIR}/external/openssl-ep")

# ── MSVC native Windows build ──────────────────────────────────────────────
if(MSVC AND CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT CMAKE_CROSSCOMPILING)
	# Need nmake (comes with Visual Studio / Build Tools).
	find_program(RETDEC_NMAKE_EXECUTABLE NAMES nmake REQUIRED
		DOC "nmake for OpenSSL VC-WIN64A build — must be on PATH (Developer Command Prompt)")

	# OpenSSL VC-WIN64A uses .lib not .a; no-asm skips NASM requirement.
	set(_RETDEC_OSSL_CRYPTO_LIB "${RETDEC_OPENSSL_INSTALL_DIR}/lib/libcrypto.lib")

	ExternalProject_Add(retdec-openssl
		PREFIX "${_RETDEC_OSSL_EP_PREFIX}"
		URL "${RETDEC_OSSL_URL}"
		URL_HASH "SHA256=${RETDEC_OSSL_SHA256}"
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		# VC-WIN64A: 64-bit MSVC target; no-shared = static libs only;
		# no-tests = skip test build; no-asm = no NASM dependency.
		CONFIGURE_COMMAND
			${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
			${PERL_EXECUTABLE} Configure VC-WIN64A
				--prefix=${RETDEC_OSSL_PREFIX}
				--openssldir=SSL
				--libdir=lib
				no-shared
				no-tests
				no-asm
		BUILD_IN_SOURCE TRUE
		# /FS lets parallel cl.exe share one PDB (needed when top-level MSBuild uses many /p jobs).
		BUILD_COMMAND
			${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
				${CMAKE_COMMAND} -E env "CL=/FS" ${RETDEC_NMAKE_EXECUTABLE}
		INSTALL_COMMAND
			${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
				${CMAKE_COMMAND} -E env "CL=/FS" ${RETDEC_NMAKE_EXECUTABLE} install_sw
		# Tell Ninja about the output file so it can track the dependency
		# without requiring a separate 'ninja retdec-openssl' pre-step.
		BUILD_BYPRODUCTS "${_RETDEC_OSSL_CRYPTO_LIB}"
	)

# ── MinGW / cross to Windows ──────────────────────────────────────────────
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	# Native MinGW (e.g. MSYS2) or Linux→Windows cross with MinGW-w64.
	find_program(RETDEC_GNUMAKE_EXECUTABLE NAMES make mingw32-make REQUIRED)

	# For MinGW cross-compilation: use ONLY --cross-compile-prefix.
	# OpenSSL's Configure prepends CROSS_COMPILE to ALL tool variables
	# (CC, AR, RANLIB…). Mixing --cross-compile-prefix with explicit CC/AR
	# doubles the prefix (e.g. x86_64-w64-mingw32-x86_64-w64-mingw32-ar).
	# So we pass NO env vars when cross-compiling.
	set(_OSSL_CROSS "")
	set(_OSSL_ENV_VARS "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}")
	if(CMAKE_AR)
		list(APPEND _OSSL_ENV_VARS "AR=${CMAKE_AR}")
	endif()
	if(CMAKE_RANLIB)
		list(APPEND _OSSL_ENV_VARS "RANLIB=${CMAKE_RANLIB}")
	endif()
	if(CMAKE_CROSSCOMPILING)
		if(CMAKE_C_COMPILER MATCHES "x86_64-w64-mingw32")
			set(_OSSL_CROSS "--cross-compile-prefix=x86_64-w64-mingw32-")
		elseif(CMAKE_C_COMPILER MATCHES "i686-w64-mingw32")
			set(_OSSL_CROSS "--cross-compile-prefix=i686-w64-mingw32-")
		else()
			set(_OSSL_CROSS "--cross-compile-prefix=x86_64-w64-mingw32-")
		endif()
		# No environment overrides — OpenSSL derives all tools from the
		# cross-compile-prefix.
		set(_OSSL_CONFIGURE_ENV "")
	else()
		set(_OSSL_CONFIGURE_ENV ${CMAKE_COMMAND} -E env ${_OSSL_ENV_VARS} --)
	endif()

	set(_RETDEC_OSSL_CRYPTO_LIB "${RETDEC_OPENSSL_INSTALL_DIR}/lib/libcrypto.a")

	ExternalProject_Add(retdec-openssl
		PREFIX "${_RETDEC_OSSL_EP_PREFIX}"
		URL "${RETDEC_OSSL_URL}"
		URL_HASH "SHA256=${RETDEC_OSSL_SHA256}"
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		CONFIGURE_COMMAND
			${_OSSL_CONFIGURE_ENV}
			${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
			${PERL_EXECUTABLE} Configure mingw64 ${_OSSL_CROSS}
				--prefix=${RETDEC_OSSL_PREFIX}
				--openssldir=SSL
				--libdir=lib
				no-shared
				no-tests
				no-asm
		BUILD_IN_SOURCE TRUE
		BUILD_COMMAND
			${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
			${RETDEC_GNUMAKE_EXECUTABLE} -j${CPUS}
		INSTALL_COMMAND
			${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
			${RETDEC_GNUMAKE_EXECUTABLE} install_sw
		BUILD_BYPRODUCTS "${_RETDEC_OSSL_CRYPTO_LIB}"
	)

# ── Linux native ────────────────────────────────────────────────────────────
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
	find_program(RETDEC_GNUMAKE_EXECUTABLE NAMES make REQUIRED)
	set(_RETDEC_OSSL_TARGET "linux-x86_64")
	if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
		set(_RETDEC_OSSL_TARGET "linux-generic64")
	endif()

	set(_RETDEC_OSSL_CRYPTO_LIB "${RETDEC_OPENSSL_INSTALL_DIR}/lib/libcrypto.a")

	ExternalProject_Add(retdec-openssl
		PREFIX "${_RETDEC_OSSL_EP_PREFIX}"
		URL "${RETDEC_OSSL_URL}"
		URL_HASH "SHA256=${RETDEC_OSSL_SHA256}"
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		CONFIGURE_COMMAND
			${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
			${PERL_EXECUTABLE} Configure ${_RETDEC_OSSL_TARGET}
				--prefix=${RETDEC_OSSL_PREFIX}
				--openssldir=SSL
				no-shared
				no-tests
		BUILD_IN_SOURCE TRUE
		BUILD_COMMAND
			${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
			${RETDEC_GNUMAKE_EXECUTABLE} -j${CPUS}
		INSTALL_COMMAND
			${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
			${RETDEC_GNUMAKE_EXECUTABLE} install_sw
		BUILD_BYPRODUCTS "${_RETDEC_OSSL_CRYPTO_LIB}"
	)

else()
	message(FATAL_ERROR
		"RETDEC_BUNDLED_OPENSSL: no build rule for this platform "
		"(CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}, "
		"CMAKE_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}, "
		"MSVC=${MSVC}). "
		"Set RETDEC_BUNDLED_OPENSSL=OFF and supply OpenSSL yourself, "
		"or use a supported toolchain (MSVC, MinGW-w64, Linux GCC/Clang).")
endif()

add_library(OpenSSL::Crypto UNKNOWN IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
	IMPORTED_LINK_INTERFACE_LANGUAGES "C"
	IMPORTED_LOCATION "${_RETDEC_OSSL_CRYPTO_LIB}"
	INTERFACE_INCLUDE_DIRECTORIES "${RETDEC_OPENSSL_INSTALL_DIR}/include"
)

set(OPENSSL_ROOT_DIR    "${RETDEC_OPENSSL_INSTALL_DIR}" CACHE PATH     "RetDec bundled OpenSSL" FORCE)
set(OPENSSL_INCLUDE_DIR "${RETDEC_OPENSSL_INSTALL_DIR}/include" CACHE PATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${_RETDEC_OSSL_CRYPTO_LIB}" CACHE FILEPATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "" CACHE FILEPATH "" FORCE)
set(OPENSSL_LIBRARIES   "${_RETDEC_OSSL_CRYPTO_LIB}" CACHE STRING "" FORCE)

message(STATUS
	"RETDEC_BUNDLED_OPENSSL: sources under ${_RETDEC_OSSL_EP_PREFIX}/src, "
	"install to ${RETDEC_OPENSSL_INSTALL_DIR}")
