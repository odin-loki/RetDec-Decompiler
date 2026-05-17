##
## tests/compiler_abi/CMakeLists.txt
##

add_executable(retdec-compiler-abi-tests
    abi_test.cpp
)

target_link_libraries(retdec-compiler-abi-tests
    PRIVATE
        retdec::compiler-abi
        retdec::deps::gtest_main
)

set_target_properties(retdec-compiler-abi-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

gtest_discover_tests(retdec-compiler-abi-tests)
