add_executable(retdec-testing-tests
    test_harness_test.cpp
)

target_include_directories(retdec-testing-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-testing-tests
    PRIVATE
        retdec-testing
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec-testing-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-testing-tests
    COMMAND retdec-testing-tests
)
