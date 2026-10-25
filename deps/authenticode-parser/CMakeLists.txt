
if(RETDEC_BUNDLED_OPENSSL)
	if(NOT TARGET OpenSSL::Crypto)
		message(FATAL_ERROR "RETDEC_BUNDLED_OPENSSL is ON but OpenSSL::Crypto was not created (add deps/openssl before authenticode-parser).")
	endif()
else()
	find_package(OpenSSL 1.1.1 REQUIRED)
endif()

add_library(authenticode STATIC
	src/authenticode.c
	src/helper.c
	src/structs.c
	src/countersignature.c
	src/certificate.c
)

add_library(retdec::deps::authenticode ALIAS authenticode)

include (TestBigEndian)
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
	target_compile_definitions(authenticode PRIVATE WORDS_BIGENDIAN)
endif()

# OpenSSL 3.x exposes legacy struct fields only below 3.0 API compat; strdup on glibc needs _GNU_SOURCE.
target_compile_definitions(authenticode PRIVATE OPENSSL_API_COMPAT=0x10100000L)
if(UNIX AND NOT APPLE)
	target_compile_definitions(authenticode PRIVATE _GNU_SOURCE)
endif()

# Disable all warnings from this 3rd party project.
if(MSVC)
	target_compile_options(authenticode PRIVATE "/w")
else()
	target_compile_options(authenticode PRIVATE "-w")
endif()

target_include_directories(authenticode
	PUBLIC
		$<BUILD_INTERFACE:${RETDEC_DEPS_DIR}/authenticode-parser/include>
		$<INSTALL_INTERFACE:${RETDEC_INSTALL_DEPS_INCLUDE_DIR}>
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(authenticode
	PRIVATE
		OpenSSL::Crypto
)

if(RETDEC_BUNDLED_OPENSSL)
	add_dependencies(authenticode retdec-openssl)
endif()

set_target_properties(authenticode
	PROPERTIES
		OUTPUT_NAME "retdec-authenticode"
)

# Install includes.
install(
	DIRECTORY ${RETDEC_DEPS_DIR}/authenticode-parser/include/
	DESTINATION ${RETDEC_INSTALL_DEPS_INCLUDE_DIR}
)

# Install libs.
install(TARGETS authenticode
	EXPORT authenticode-targets
	ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
	LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)

# Export targets.
install(EXPORT authenticode-targets
	FILE "retdec-authenticode-targets.cmake"
	NAMESPACE retdec::deps::
	DESTINATION ${RETDEC_INSTALL_CMAKE_DIR}
)

# Install CMake files.
install(
	FILES
		"${CMAKE_CURRENT_LIST_DIR}/retdec-authenticode-config.cmake"
	DESTINATION
		"${RETDEC_INSTALL_CMAKE_DIR}"
)
