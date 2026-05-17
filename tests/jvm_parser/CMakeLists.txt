add_executable(retdec-jvm-parser-tests
    jvm_parser_test.cpp
)

target_link_libraries(retdec-jvm-parser-tests
    PRIVATE
        retdec-jvm-parser
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec-jvm-parser-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME retdec-jvm-parser-tests COMMAND retdec-jvm-parser-tests)
