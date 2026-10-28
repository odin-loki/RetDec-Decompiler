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

function(_retdec_openssl_apply_crypto_only_patch _bundle_dir)
	set(_top "${_bundle_dir}/CMakeLists.txt")
	if(NOT EXISTS "${_top}")
		return()
	endif()
	file(READ "${_top}" _txt)
	foreach(_subdir IN ITEMS ssl apps engines tools exporters)
		string(REPLACE "add_subdirectory(${_subdir})"
			"# RetDec: libcrypto only\n# add_subdirectory(${_subdir})"
			_txt "${_txt}")
	endforeach()
	if(NOT _txt MATCHES "RetDec crypto-only: fipskey.h")
		set(_fipskey_snippet [[
# RetDec crypto-only: fipskey.h is normally generated under apps/.
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/apps/fipskey.h.cmake.in"
	"${CMAKE_BINARY_DIR}/include/openssl/fipskey.h" @ONLY)
]])
		string(REPLACE "add_subdirectory(crypto)" "${_fipskey_snippet}\nadd_subdirectory(crypto)" _txt "${_txt}")
	endif()
	file(WRITE "${_top}" "${_txt}")
endfunction()

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

# A restored CI bundle was copied from an older wrapper. These inserts are
# idempotent so the early-return path still picks up OpenSSL 3.5 sources the
# Fsu0413 lists omit (hashfunc, skeymgmt, the RFC 5755 v3 extensions, ssl_err).
function(_retdec_openssl_ensure_source _file _anchor _rel)
	if(NOT EXISTS "${_file}")
		message(WARNING "OpenSSL bundle patch: missing ${_file}")
		return()
	endif()
	file(READ "${_file}" _txt)
	string(FIND "${_txt}" "${_rel}" _found)
	if(NOT _found EQUAL -1)
		return()
	endif()
	string(FIND "${_txt}" "${_anchor}" _anchor_at)
	if(_anchor_at EQUAL -1)
		message(WARNING "OpenSSL bundle patch: ${_file} has no anchor ${_anchor}")
		return()
	endif()
	string(CONCAT _with "${_anchor}" "
    \${CMAKE_SOURCE_DIR}/openssl/${_rel}")
	string(REPLACE "${_anchor}" "${_with}" _txt "${_txt}")
	file(WRITE "${_file}" "${_txt}")
endfunction()

function(_retdec_openssl_ensure_cmakedefine _file _name)
	if(NOT EXISTS "${_file}")
		return()
	endif()
	file(READ "${_file}" _txt)
	string(FIND "${_txt}" "#cmakedefine ${_name}" _found)
	if(NOT _found EQUAL -1)
		return()
	endif()
	string(FIND "${_txt}" "#cmakedefine OPENSSL_NO_MODES" _anchor_at)
	if(_anchor_at EQUAL -1)
		message(WARNING "OpenSSL bundle patch: ${_file} has no OPENSSL_NO_MODES anchor")
		return()
	endif()
	string(CONCAT _with "#cmakedefine ${_name}" "
#cmakedefine OPENSSL_NO_MODES")
	string(REPLACE "#cmakedefine OPENSSL_NO_MODES" "${_with}" _txt "${_txt}")
	file(WRITE "${_file}" "${_txt}")
endfunction()

function(_retdec_openssl_apply_35_link_fixes _bundle_dir)
	_retdec_openssl_ensure_source(
		"${_bundle_dir}/crypto/hashtable/objects.cmake"
		"openssl/crypto/hashtable/hashtable.c"
		"crypto/hashtable/hashfunc.c")
	_retdec_openssl_ensure_source(
		"${_bundle_dir}/crypto/err/objects.cmake"
		"openssl/crypto/err/err.c"
		"crypto/ssl_err.c")
	_retdec_openssl_ensure_source(
		"${_bundle_dir}/crypto/evp/objects.cmake"
		"openssl/crypto/evp/evp_enc.c"
		"crypto/evp/s_lib.c")
	# s_lib.c calls EVP_SKEYMGMT_fetch and the rest of this table. The
	# provider skeymgmt objects do not define it.
	_retdec_openssl_ensure_source(
		"${_bundle_dir}/crypto/evp/objects.cmake"
		"openssl/crypto/evp/s_lib.c"
		"crypto/evp/skeymgmt_meth.c")
	foreach(_v3 IN ITEMS v3_authattid.c v3_rolespec.c v3_attrdesc.c v3_timespec.c v3_attrmap.c v3_aaa.c)
		_retdec_openssl_ensure_source(
			"${_bundle_dir}/crypto/x509/objects.cmake"
			"openssl/crypto/x509/v3_iobo.c"
			"crypto/x509/${_v3}")
	endforeach()
	_retdec_openssl_ensure_source(
		"${_bundle_dir}/crypto/CMakeLists.txt"
		"openssl/providers/defltprov.c"
		"providers/implementations/skeymgmt/aes_skmgmt.c")
	_retdec_openssl_ensure_source(
		"${_bundle_dir}/crypto/CMakeLists.txt"
		"openssl/providers/defltprov.c"
		"providers/implementations/skeymgmt/generic.c")
	set(_cfg "${_bundle_dir}/crypto/configuration.h.cmake.in")
	foreach(_name IN ITEMS OPENSSL_NO_FIPS_JITTER OPENSSL_NO_ML_DSA OPENSSL_NO_ML_KEM OPENSSL_NO_SLH_DSA)
		_retdec_openssl_ensure_cmakedefine("${_cfg}" "${_name}")
	endforeach()
endfunction()

set(_bundle_ready_marker "${BUNDLE_DIR}/pregenerated/include/openssl/core_names.h")
if(EXISTS "${_bundle_ready_marker}" AND NOT "$ENV{RETDEC_FORCE_OPENSSL_BOOTSTRAP}" STREQUAL "1"
	AND NOT "$ENV{OPENSSL_FORCE_BOOTSTRAP}" STREQUAL "1")
	_retdec_openssl_apply_crypto_only_patch("${BUNDLE_DIR}")
	_retdec_openssl_apply_35_link_fixes("${BUNDLE_DIR}")
	_retdec_openssl_write_paramnames_stub("${BUNDLE_DIR}")
	message(STATUS "OpenSSL bundle already present at ${BUNDLE_DIR} (refreshed patches; set RETDEC_FORCE_OPENSSL_BOOTSTRAP=1 to rebuild)")
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

_retdec_openssl_apply_35_link_fixes("${BUNDLE_DIR}")

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

_retdec_openssl_apply_crypto_only_patch("${BUNDLE_DIR}")
_retdec_openssl_write_paramnames_stub("${BUNDLE_DIR}")

message(STATUS "OpenSSL CMake bundle ready at ${BUNDLE_DIR}")
