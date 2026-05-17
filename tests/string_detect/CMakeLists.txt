add_executable(retdec-string-detect-tests
    string_detect_test.cpp
)

target_link_libraries(retdec-string-detect-tests
    PRIVATE
        retdec-string-detect
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_include_directories(retdec-string-detect-tests
    PRIVATE
        ${RETDEC_INCLUDE_DIR}
)

gtest_discover_tests(retdec-string-detect-tests)
