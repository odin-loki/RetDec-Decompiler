
add_executable(tests-utils
	alignment_tests.cpp
	array_tests.cpp
	binary_path_tests.cpp
	byte_value_storage_kernel_tests.cpp
	byte_value_storage_tests.cpp
	container_tests.cpp
	conversion_tests.cpp
	dynamic_buffer_tests.cpp
	equality_tests.cpp
	filter_iterator_tests.cpp
	gtest_shim_conformance_tests.cpp
	gpu_scanner_tests.cpp
	math_tests.cpp
	memory_tests.cpp
	scope_exit_tests.cpp
	string_tests.cpp
	time_tests.cpp
	version_tests.cpp
)

target_link_libraries(tests-utils
	retdec::utils
	retdec::gpu-scanner
	retdec::deps::gmock_main
)

set_target_properties(tests-utils
	PROPERTIES
		OUTPUT_NAME "retdec-tests-utils"
)

install(TARGETS tests-utils
	RUNTIME DESTINATION ${RETDEC_INSTALL_TESTS_DIR}
)
