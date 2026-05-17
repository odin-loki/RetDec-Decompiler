add_executable(retdec-code-data-tests
    code_data_classifier_test.cpp
)

target_link_libraries(retdec-code-data-tests
    PRIVATE
        retdec::code-data
        retdec::deps::gtest_main
)

set_target_properties(retdec-code-data-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-code-data-tests)
