
add_executable(tests-cpdetect
	packer_section_hints_tests.cpp
)

target_include_directories(tests-cpdetect
	PRIVATE
		${RETDEC_TESTS_DIR}
)

target_link_libraries(tests-cpdetect
	retdec::utils
	retdec::deps::gmock_main
)

set_target_properties(tests-cpdetect
	PROPERTIES
		OUTPUT_NAME "retdec-tests-cpdetect"
)

install(TARGETS tests-cpdetect
	RUNTIME DESTINATION ${RETDEC_INSTALL_TESTS_DIR}
)
