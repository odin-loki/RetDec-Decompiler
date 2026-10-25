# Assemble Fsu0413/openssl-externalCMake + official OpenSSL tarball into one source tree.
# Invoked from ExternalProject DOWNLOAD_COMMAND (no Perl / Configure).

cmake_minimum_required(VERSION 3.26)

if(NOT DEFINED OPENSSL_URL OR NOT DEFINED OPENSSL_ARCHIVE_SHA256
	OR NOT DEFINED OPENSSL_EXTERNAL_CMAKE_URL OR NOT DEFINED OPENSSL_EXTERNAL_CMAKE_ARCHIVE_SHA256
	OR NOT DEFINED BUNDLE_DIR OR NOT DEFINED OPENSSL_VERSION)
	message(FATAL_ERROR
		"assemble_openssl_cmake_bundle.cmake: missing OPENSSL_URL, hashes, BUNDLE_DIR, or OPENSSL_VERSION")
endif()

get_filename_component(_dl_parent "${BUNDLE_DIR}" DIRECTORY)
set(_download_dir "${_dl_parent}/download")
file(MAKE_DIRECTORY "${_download_dir}")

set(_ossl_tar "${_download_dir}/openssl-${OPENSSL_VERSION}.tar.gz")
if(NOT EXISTS "${_ossl_tar}")
	message(STATUS "Downloading OpenSSL ${OPENSSL_VERSION} source...")
	file(DOWNLOAD "${OPENSSL_URL}" "${_ossl_tar}"
		EXPECTED_HASH SHA256=${OPENSSL_ARCHIVE_SHA256}
		SHOW_PROGRESS)
endif()

set(_cmake_zip "${_download_dir}/openssl-external-cmake.zip")
if(NOT EXISTS "${_cmake_zip}")
	message(STATUS "Downloading openssl-externalCMake...")
	file(DOWNLOAD "${OPENSSL_EXTERNAL_CMAKE_URL}" "${_cmake_zip}"
		EXPECTED_HASH SHA256=${OPENSSL_EXTERNAL_CMAKE_ARCHIVE_SHA256}
		SHOW_PROGRESS)
endif()

file(REMOVE_RECURSE "${BUNDLE_DIR}")
file(MAKE_DIRECTORY "${BUNDLE_DIR}")

set(_stage "${_download_dir}/external-cmake-stage")
file(REMOVE_RECURSE "${_stage}")
file(MAKE_DIRECTORY "${_stage}")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E tar x "${_cmake_zip}"
	WORKING_DIRECTORY "${_stage}"
	RESULT_VARIABLE _untar_ec
)
if(_untar_ec)
	message(FATAL_ERROR "Failed to extract ${_cmake_zip} (exit ${_untar_ec})")
endif()

file(GLOB _cmake_roots RELATIVE "${_stage}" "${_stage}/openssl-externalCMake-*")
list(LENGTH _cmake_roots _nroots)
if(NOT _nroots EQUAL 1)
	message(FATAL_ERROR "Expected one openssl-externalCMake-* directory in ${_stage}, got: ${_cmake_roots}")
endif()
set(_cmake_root "${_stage}/${_cmake_roots}")

file(GLOB _ossl_extracted RELATIVE "${_download_dir}" "${_download_dir}/openssl-${OPENSSL_VERSION}")
if(NOT EXISTS "${_download_dir}/openssl-${OPENSSL_VERSION}")
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E tar xzf "${_ossl_tar}"
		WORKING_DIRECTORY "${_download_dir}"
		RESULT_VARIABLE _ossl_tar_ec
	)
	if(_ossl_tar_ec)
		message(FATAL_ERROR "Failed to extract ${_ossl_tar} (exit ${_ossl_tar_ec})")
	endif()
endif()
if(NOT IS_DIRECTORY "${_download_dir}/openssl-${OPENSSL_VERSION}")
	message(FATAL_ERROR "OpenSSL source dir missing after extract: ${_download_dir}/openssl-${OPENSSL_VERSION}")
endif()

# Copy external CMake project into BUNDLE_DIR (exclude nested openssl submodule checkout).
file(GLOB _cmake_entries RELATIVE "${_cmake_root}" "${_cmake_root}/*")
foreach(_entry IN LISTS _cmake_entries)
	if(_entry STREQUAL "openssl")
		continue()
	endif()
	file(COPY "${_cmake_root}/${_entry}" DESTINATION "${BUNDLE_DIR}")
endforeach()

file(MAKE_DIRECTORY "${BUNDLE_DIR}/openssl")
file(COPY "${_download_dir}/openssl-${OPENSSL_VERSION}/" DESTINATION "${BUNDLE_DIR}/openssl")

if(NOT EXISTS "${BUNDLE_DIR}/openssl/VERSION.dat")
	message(FATAL_ERROR "Assembled bundle missing openssl/VERSION.dat under ${BUNDLE_DIR}")
endif()
if(NOT EXISTS "${BUNDLE_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "Assembled bundle missing top-level CMakeLists.txt under ${BUNDLE_DIR}")
endif()

# OpenSSL 3.5+ tarballs omit generated headers and ssl_err.c; openssl-externalCMake
# still lists them. Run Configure + build_generated once (Perl only, not the final link).
set(_ossl_src "${BUNDLE_DIR}/openssl")
if(CMAKE_HOST_WIN32)
	# OpenSSL Configure needs a full Perl (Strawberry on CI); Git's msys perl is too minimal.
	find_program(_RETDEC_OSSL_PERL NAMES perl.exe
		PATHS "C:/Strawberry/perl/bin" "C:/Program Files/Git/usr/bin"
		NO_DEFAULT_PATH)
	if(NOT _RETDEC_OSSL_PERL)
		find_program(_RETDEC_OSSL_PERL NAMES perl perl.exe)
	endif()
else()
	find_program(_RETDEC_OSSL_PERL NAMES perl)
endif()
if(NOT _RETDEC_OSSL_PERL)
	message(FATAL_ERROR "OpenSSL bundle assembly needs Perl (Git for Windows usr/bin/perl.exe on Windows).")
endif()

if(CMAKE_HOST_WIN32)
	set(_ossl_cfg_target "VC-WIN64A")
	find_program(_RETDEC_OSSL_NMAKE nmake)
	if(NOT _RETDEC_OSSL_NMAKE)
		message(FATAL_ERROR "OpenSSL bundle assembly on Windows needs nmake (Visual Studio).")
	endif()
	set(_ossl_gen_cmd ${_RETDEC_OSSL_NMAKE} build_generated)
elseif(CMAKE_HOST_APPLE)
	execute_process(COMMAND uname -m OUTPUT_VARIABLE _host_cpu OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(_host_cpu STREQUAL "arm64")
		set(_ossl_cfg_target "darwin64-arm64-cc")
	else()
		set(_ossl_cfg_target "darwin64-x86_64-cc")
	endif()
	find_program(_RETDEC_OSSL_MAKE NAMES gmake make)
	set(_ossl_gen_cmd ${_RETDEC_OSSL_MAKE} build_generated)
else()
	set(_ossl_cfg_target "linux-x86_64")
	find_program(_RETDEC_OSSL_MAKE NAMES gmake make)
	set(_ossl_gen_cmd ${_RETDEC_OSSL_MAKE} build_generated)
endif()

message(STATUS "OpenSSL ${OPENSSL_VERSION}: running Configure ${_ossl_cfg_target} (generated sources only)...")
execute_process(
	COMMAND ${_RETDEC_OSSL_PERL} Configure ${_ossl_cfg_target} no-asm no-shared no-tests no-ui-console
	WORKING_DIRECTORY "${_ossl_src}"
	RESULT_VARIABLE _ossl_cfg_ec
)
if(_ossl_cfg_ec)
	message(FATAL_ERROR "OpenSSL Configure failed (exit ${_ossl_cfg_ec}) while assembling bundle.")
endif()

if(NOT _ossl_gen_cmd)
	message(FATAL_ERROR "OpenSSL bundle assembly needs make or nmake for build_generated.")
endif()
message(STATUS "OpenSSL ${OPENSSL_VERSION}: running build_generated...")
execute_process(
	COMMAND ${_ossl_gen_cmd}
	WORKING_DIRECTORY "${_ossl_src}"
	RESULT_VARIABLE _ossl_gen_ec
)
if(_ossl_gen_ec)
	message(FATAL_ERROR "OpenSSL build_generated failed (exit ${_ossl_gen_ec}) while assembling bundle.")
endif()

if(EXISTS "${_ossl_src}/crypto/ssl_err.c" AND NOT EXISTS "${_ossl_src}/ssl/ssl_err.c")
	file(COPY "${_ossl_src}/crypto/ssl_err.c" DESTINATION "${_ossl_src}/ssl/")
endif()

# openssl-externalCMake lists ssl/sslerr.h; OpenSSL 3.5 ships it under include/openssl/.
if(NOT EXISTS "${_ossl_src}/ssl/sslerr.h")
	if(EXISTS "${_ossl_src}/include/openssl/sslerr.h")
		file(COPY "${_ossl_src}/include/openssl/sslerr.h" DESTINATION "${_ossl_src}/ssl/")
	else()
		message(FATAL_ERROR
			"OpenSSL bundle assembly: missing sslerr.h (expected ssl/ or include/openssl/).")
	endif()
endif()

set(_ossl_crypto_cmake "${BUNDLE_DIR}/crypto/CMakeLists.txt")
if(EXISTS "${_ossl_crypto_cmake}")
	file(READ "${_ossl_crypto_cmake}" _ossl_crypto_cmake_txt)
	string(REPLACE "\${CMAKE_SOURCE_DIR}/openssl/include/openssl/asn1_mac.h" ""
		_ossl_crypto_cmake_txt "${_ossl_crypto_cmake_txt}")
	file(WRITE "${_ossl_crypto_cmake}" "${_ossl_crypto_cmake_txt}")
endif()

message(STATUS "OpenSSL CMake bundle ready at ${BUNDLE_DIR}")
