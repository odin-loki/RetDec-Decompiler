
add_executable(tests-pelib
	pelib_test.cpp
)

target_link_libraries(tests-pelib
	retdec::pelib
	retdec::deps::gmock_main
)

set_target_properties(tests-pelib
	PROPERTIES
		OUTPUT_NAME "retdec-tests-pelib"
)

install(TARGETS tests-pelib
	RUNTIME DESTINATION ${RETDEC_INSTALL_TESTS_DIR}
)
