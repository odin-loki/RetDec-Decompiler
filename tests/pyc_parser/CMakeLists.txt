add_executable(retdec-pyc-parser-tests
    pyc_parser_test.cpp
)

target_include_directories(retdec-pyc-parser-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-pyc-parser-tests
    PRIVATE
        retdec-pyc-parser
        retdec-bc-module
        gtest
        gtest_main
)

set_target_properties(retdec-pyc-parser-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME pyc-parser-tests COMMAND retdec-pyc-parser-tests)
