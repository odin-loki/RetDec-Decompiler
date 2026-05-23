
add_library(retdec STATIC
    retdec.cpp
    llvm_to_ssa.cpp
    function_analysis_cache.cpp
    semantic_recovery_export.cpp
)
add_library(retdec::retdec ALIAS retdec)

target_compile_features(retdec PUBLIC cxx_std_17)

target_include_directories(retdec
	PUBLIC
		$<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
		$<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

target_link_libraries(retdec
	PUBLIC
		retdec::common
		retdec::deps::capstone
		retdec::deps::llvm
		# Post-decompile analysis passes (PUBLIC: symbols used in retdec static lib)
		retdec-concurrency-detect
		retdec_sort_detect
		retdec_container_detect
		retdec_algo_recover
		retdec-type-inference
		retdec-serial-detect
		retdec-ipa
		retdec-call-conv
		retdec-ptx-decompile
		retdec-ssa
	PRIVATE
		retdec::bin2llvmir
		retdec::llvmir2hll
		retdec::llvmir-emul
		retdec::config
		Threads::Threads
		retdec::gpu-scanner
		retdec::deps::rapidjson
)

set_target_properties(retdec
	PROPERTIES
		OUTPUT_NAME "retdec-retdec"
)

# Install includes.
install(
	DIRECTORY ${RETDEC_INCLUDE_DIR}/retdec/retdec
	DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec
)

# Install libs.
install(TARGETS retdec
	EXPORT retdec-targets
	ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
	LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)

# Export targets.
install(EXPORT retdec-targets
	FILE "retdec-retdec-targets.cmake"
	NAMESPACE retdec::
	DESTINATION ${RETDEC_INSTALL_CMAKE_DIR}
)

# Install CMake files.
configure_file(
	"retdec-retdec-config.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/retdec-retdec-config.cmake"
	@ONLY
)
install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}/retdec-retdec-config.cmake"
	DESTINATION
		"${RETDEC_INSTALL_CMAKE_DIR}"
)
