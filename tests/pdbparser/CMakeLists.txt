
add_executable(tests-pdbparser
	pdbparser_test.cpp
)

target_link_libraries(tests-pdbparser
	retdec::pdbparser
	retdec::deps::gmock_main
)

set_target_properties(tests-pdbparser
	PROPERTIES
		OUTPUT_NAME "retdec-tests-pdbparser"
)

install(TARGETS tests-pdbparser
	RUNTIME DESTINATION ${RETDEC_INSTALL_TESTS_DIR}
)
