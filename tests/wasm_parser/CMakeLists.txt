##
## CMakeLists.txt -- retdec-wasm-parser-tests
##

add_executable(retdec-wasm-parser-tests
    wasm_parser_test.cpp
)

target_link_libraries(retdec-wasm-parser-tests
    PRIVATE
        retdec-wasm-parser
        gtest_main
)

add_test(NAME retdec-wasm-parser-tests
    COMMAND retdec-wasm-parser-tests
)
