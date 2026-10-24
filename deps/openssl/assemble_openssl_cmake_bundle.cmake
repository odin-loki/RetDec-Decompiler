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

message(STATUS "OpenSSL CMake bundle ready at ${BUNDLE_DIR}")
