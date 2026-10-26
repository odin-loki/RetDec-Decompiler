# Assemble vendored Fsu0413/openssl-externalCMake wrapper + openssl-src into bundle/openssl.
# Run once (or when vendored inputs change): cmake -P scripts/bootstrap_openssl_bundle.cmake
# Optional: -DBUNDLE_DIR=...  RETDEC_FORCE_OPENSSL_BOOTSTRAP=1 to rebuild an existing bundle.

cmake_minimum_required(VERSION 3.26)

get_filename_component(_RETDEC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED BUNDLE_DIR)
	set(BUNDLE_DIR "${_RETDEC_ROOT}/deps/openssl/vendored/bundle")
endif()
get_filename_component(BUNDLE_DIR "${BUNDLE_DIR}" ABSOLUTE)

set(_VENDORED "${_RETDEC_ROOT}/deps/openssl/vendored")
set(_WRAPPER "${_VENDORED}/wrapper")
set(_OPENSSL_SRC_VENDOR "${_VENDORED}/openssl-src")

if(NOT EXISTS "${_RETDEC_ROOT}/cmake/deps.cmake")
	message(FATAL_ERROR "bootstrap_openssl_bundle.cmake: missing ${_RETDEC_ROOT}/cmake/deps.cmake")
endif()
include("${_RETDEC_ROOT}/cmake/deps.cmake")

if(NOT IS_DIRECTORY "${_WRAPPER}")
	message(FATAL_ERROR
		"bootstrap_openssl_bundle.cmake: missing wrapper at ${_WRAPPER}")
endif()
if(NOT EXISTS "${_WRAPPER}/CMakeLists.txt")
	message(FATAL_ERROR
		"bootstrap_openssl_bundle.cmake: ${_WRAPPER}/CMakeLists.txt missing")
endif()
if(NOT IS_DIRECTORY "${_OPENSSL_SRC_VENDOR}")
	message(FATAL_ERROR
		"bootstrap_openssl_bundle.cmake: openssl-src submodule missing — "
		"git submodule update --init deps/openssl/vendored/openssl-src")
endif()
if(NOT EXISTS "${_OPENSSL_SRC_VENDOR}/VERSION.dat")
	message(FATAL_ERROR
		"bootstrap_openssl_bundle.cmake: ${_OPENSSL_SRC_VENDOR}/VERSION.dat missing (wrong submodule ref?)")
endif()

function(_retdec_openssl_write_paramnames_stub _bundle_dir)
	set(_ossl_paramnames_cmake "${_bundle_dir}/crypto/paramnames.cmake")
	file(MAKE_DIRECTORY "${_bundle_dir}/crypto")
	file(WRITE "${_ossl_paramnames_cmake}" [[# SPDX-License-Identifier: Unlicense
# RetDec: copy param-name artifacts from bootstrap-time build_generated (no nested Perl).
cmake_minimum_required(VERSION 3.13)
if(NOT DEFINED OUTPUT_FILE_NAME)
	message(FATAL_ERROR "paramnames.cmake: OUTPUT_FILE_NAME is required")
endif()
# Match openssl-externalCMake: on Windows CMAKE_ARGV includes -D... before -P, so argv1 is not the .in path.
set(_INPUT_ARGN)
foreach(_I RANGE ${CMAKE_ARGC})
	if(CMAKE_ARGV${_I} STREQUAL "-P")
		math(EXPR _INPUT_ARGN "${_I} + 2")
		break()
	endif()
endforeach()
if(NOT DEFINED _INPUT_ARGN)
	message(FATAL_ERROR "paramnames.cmake: expected -P script invocation")
endif()
get_filename_component(_INPUT_IN "${CMAKE_ARGV${_INPUT_ARGN}}" ABSOLUTE)
get_filename_component(_OSSL_SRC "${CMAKE_CURRENT_LIST_DIR}/../openssl" ABSOLUTE)
file(RELATIVE_PATH _INPUT_REL "${_OSSL_SRC}" "${_INPUT_IN}")
string(REPLACE ".in" "" _PREGEN_REL "${_INPUT_REL}")
string(REPLACE "\\" "/" _PREGEN_REL "${_PREGEN_REL}")
set(_PREGEN_FILE "${CMAKE_CURRENT_LIST_DIR}/../pregenerated/${_PREGEN_REL}")
if(NOT EXISTS "${_PREGEN_FILE}")
	message(FATAL_ERROR "paramnames.cmake: missing pregenerated ${_PREGEN_REL}")
endif()
if(IS_ABSOLUTE "${OUTPUT_FILE_NAME}")
	set(_OUT_FILE "${OUTPUT_FILE_NAME}")
else()
	set(_OUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_NAME}")
endif()
get_filename_component(_OUT_DIR "${_OUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${_OUT_DIR}")
file(COPY "${_PREGEN_FILE}" DESTINATION "${_OUT_DIR}")
]])
endfunction()

set(_bundle_ready_marker "${BUNDLE_DIR}/pregenerated/include/openssl/core_names.h")
if(EXISTS "${_bundle_ready_marker}" AND NOT "$ENV{RETDEC_FORCE_OPENSSL_BOOTSTRAP}" STREQUAL "1"
	AND NOT "$ENV{OPENSSL_FORCE_BOOTSTRAP}" STREQUAL "1")
	_retdec_openssl_write_paramnames_stub("${BUNDLE_DIR}")
	message(STATUS "OpenSSL bundle already present at ${BUNDLE_DIR} (refreshed paramnames.cmake; set RETDEC_FORCE_OPENSSL_BOOTSTRAP=1 to rebuild)")
	return()
endif()

file(REMOVE_RECURSE "${BUNDLE_DIR}")
file(MAKE_DIRECTORY "${BUNDLE_DIR}")

file(GLOB _cmake_entries RELATIVE "${_WRAPPER}" "${_WRAPPER}/*")
foreach(_entry IN LISTS _cmake_entries)
	if(_entry STREQUAL "openssl")
		continue()
	endif()
	file(COPY "${_WRAPPER}/${_entry}" DESTINATION "${BUNDLE_DIR}")
endforeach()

file(MAKE_DIRECTORY "${BUNDLE_DIR}/openssl")
file(COPY "${_OPENSSL_SRC_VENDOR}/" DESTINATION "${BUNDLE_DIR}/openssl")

if(NOT EXISTS "${BUNDLE_DIR}/openssl/VERSION.dat")
	message(FATAL_ERROR "Assembled bundle missing openssl/VERSION.dat under ${BUNDLE_DIR}")
endif()
if(NOT EXISTS "${BUNDLE_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "Assembled bundle missing top-level CMakeLists.txt under ${BUNDLE_DIR}")
endif()

set(_ossl_src "${BUNDLE_DIR}/openssl")

if(CMAKE_HOST_WIN32)
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
	message(FATAL_ERROR "OpenSSL bundle bootstrap needs Perl (Git for Windows usr/bin/perl.exe on Windows).")
endif()

if(CMAKE_HOST_WIN32)
	set(_ossl_cfg_target "VC-WIN64A")
	find_program(_RETDEC_OSSL_NMAKE nmake)
	if(NOT _RETDEC_OSSL_NMAKE)
		message(FATAL_ERROR "OpenSSL bundle bootstrap on Windows needs nmake (Visual Studio).")
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
	COMMAND ${_RETDEC_OSSL_PERL} Configure ${_ossl_cfg_target} no-asm no-shared no-tests no-ui-console no-ml-dsa
	WORKING_DIRECTORY "${_ossl_src}"
	RESULT_VARIABLE _ossl_cfg_ec
)
if(_ossl_cfg_ec)
	message(FATAL_ERROR "OpenSSL Configure failed (exit ${_ossl_cfg_ec}) while bootstrapping bundle.")
endif()

if(NOT _ossl_gen_cmd)
	message(FATAL_ERROR "OpenSSL bundle bootstrap needs make or nmake for build_generated.")
endif()
message(STATUS "OpenSSL ${OPENSSL_VERSION}: running build_generated...")
execute_process(
	COMMAND ${_ossl_gen_cmd}
	WORKING_DIRECTORY "${_ossl_src}"
	RESULT_VARIABLE _ossl_gen_ec
)
if(_ossl_gen_ec)
	message(FATAL_ERROR "OpenSSL build_generated failed (exit ${_ossl_gen_ec}) while bootstrapping bundle.")
endif()

if(EXISTS "${_ossl_src}/crypto/ssl_err.c" AND NOT EXISTS "${_ossl_src}/ssl/ssl_err.c")
	file(COPY "${_ossl_src}/crypto/ssl_err.c" DESTINATION "${_ossl_src}/ssl/")
endif()

if(NOT EXISTS "${_ossl_src}/ssl/sslerr.h")
	if(EXISTS "${_ossl_src}/include/openssl/sslerr.h")
		file(COPY "${_ossl_src}/include/openssl/sslerr.h" DESTINATION "${_ossl_src}/ssl/")
	else()
		message(FATAL_ERROR
			"OpenSSL bundle bootstrap: missing sslerr.h (expected ssl/ or include/openssl/).")
	endif()
endif()

set(_ossl_cfg_h_in "${BUNDLE_DIR}/crypto/configuration.h.cmake.in")
if(EXISTS "${_ossl_cfg_h_in}")
	file(READ "${_ossl_cfg_h_in}" _ossl_cfg_h_in_txt)
	if(NOT _ossl_cfg_h_in_txt MATCHES "OPENSSL_NO_ML_DSA")
		string(REPLACE "#cmakedefine OPENSSL_NO_MODES"
			"#cmakedefine OPENSSL_NO_ML_DSA\n#cmakedefine OPENSSL_NO_MODES"
			_ossl_cfg_h_in_txt "${_ossl_cfg_h_in_txt}")
		file(WRITE "${_ossl_cfg_h_in}" "${_ossl_cfg_h_in_txt}")
	endif()
endif()

set(_ossl_crypto_cmake "${BUNDLE_DIR}/crypto/CMakeLists.txt")
if(EXISTS "${_ossl_crypto_cmake}")
	file(READ "${_ossl_crypto_cmake}" _ossl_crypto_cmake_txt)
	string(REPLACE "\${CMAKE_SOURCE_DIR}/openssl/include/openssl/asn1_mac.h" ""
		_ossl_crypto_cmake_txt "${_ossl_crypto_cmake_txt}")
	file(WRITE "${_ossl_crypto_cmake}" "${_ossl_crypto_cmake_txt}")
endif()

foreach(_pregen_rel IN ITEMS
	"include/openssl/core_names.h"
	"include/internal/param_names.h"
	"crypto/params_idx.c"
)
	if(NOT EXISTS "${_ossl_src}/${_pregen_rel}")
		message(FATAL_ERROR
			"OpenSSL bundle bootstrap: missing ${_pregen_rel} after build_generated")
	endif()
	get_filename_component(_pregen_parent "${_pregen_rel}" DIRECTORY)
	file(MAKE_DIRECTORY "${BUNDLE_DIR}/pregenerated/${_pregen_parent}")
	file(COPY "${_ossl_src}/${_pregen_rel}" DESTINATION "${BUNDLE_DIR}/pregenerated/${_pregen_parent}")
endforeach()

_retdec_openssl_write_paramnames_stub("${BUNDLE_DIR}")

message(STATUS "OpenSSL CMake bundle ready at ${BUNDLE_DIR}")
