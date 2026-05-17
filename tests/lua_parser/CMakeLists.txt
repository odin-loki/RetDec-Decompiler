##
## CMakeLists.txt -- retdec-lua-parser-tests
##

add_executable(retdec-lua-parser-tests
    lua_parser_test.cpp
)

target_link_libraries(retdec-lua-parser-tests
    PRIVATE
        retdec-lua-parser
        gtest_main
)

add_test(NAME retdec-lua-parser-tests
    COMMAND retdec-lua-parser-tests
)
