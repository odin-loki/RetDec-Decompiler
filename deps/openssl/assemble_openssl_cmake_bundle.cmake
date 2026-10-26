# Legacy entry point (ExternalProject DOWNLOAD_COMMAND). Forwards to scripts/bootstrap_openssl_bundle.cmake.
cmake_minimum_required(VERSION 3.26)

if(NOT DEFINED BUNDLE_DIR)
	get_filename_component(BUNDLE_DIR "${CMAKE_CURRENT_LIST_DIR}/vendored/bundle" ABSOLUTE)
endif()

get_filename_component(_RETDEC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_bootstrap "${_RETDEC_ROOT}/scripts/bootstrap_openssl_bundle.cmake")
if(NOT EXISTS "${_bootstrap}")
	message(FATAL_ERROR "assemble_openssl_cmake_bundle.cmake: missing ${_bootstrap}")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" -DBUNDLE_DIR=${BUNDLE_DIR} -P "${_bootstrap}"
	RESULT_VARIABLE _bootstrap_ec
)
if(_bootstrap_ec)
	message(FATAL_ERROR "bootstrap_openssl_bundle.cmake failed (exit ${_bootstrap_ec})")
endif()
