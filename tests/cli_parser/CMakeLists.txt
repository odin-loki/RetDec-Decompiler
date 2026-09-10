add_executable(retdec-cli-parser-tests
    cli_parser_test.cpp
    cli_sig_width_test.cpp
    cli_heaps_regression_test.cpp
    cli_reader_regression_test.cpp
    cil_lifter_regression_test.cpp
)

target_include_directories(retdec-cli-parser-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-cli-parser-tests
    PRIVATE
        retdec-cli-parser
        gtest
        gtest_main
)

set_target_properties(retdec-cli-parser-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-cli-parser-tests
         COMMAND retdec-cli-parser-tests)
