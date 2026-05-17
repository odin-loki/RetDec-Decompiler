add_executable(retdec-dex-parser-tests
    dex_parser_test.cpp
)

target_link_libraries(retdec-dex-parser-tests
    PRIVATE retdec-dex-parser
            retdec::deps::gtest
            retdec::deps::gtest_main
)

set_target_properties(retdec-dex-parser-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-dex-parser-tests
    COMMAND retdec-dex-parser-tests
)
