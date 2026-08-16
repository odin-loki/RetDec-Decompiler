
set(XSIMD_INSTALL_DIR ${CMAKE_BINARY_DIR}/deps/install/xsimd)

ExternalProject_Add(xsimd-project
	URL ${XSIMD_URL}
	URL_HASH SHA256=${XSIMD_ARCHIVE_SHA256}
	DOWNLOAD_NAME xsimd.zip
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ""
	INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory
		<SOURCE_DIR>/include ${XSIMD_INSTALL_DIR}/include
)

add_library(xsimd INTERFACE)
add_dependencies(xsimd xsimd-project)
add_library(retdec::deps::xsimd ALIAS xsimd)

target_include_directories(xsimd
	SYSTEM INTERFACE
		${XSIMD_INSTALL_DIR}/include
)
