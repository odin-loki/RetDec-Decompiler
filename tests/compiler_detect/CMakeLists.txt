add_executable(retdec-compiler-detect-tests
    compiler_fingerprinter_test.cpp
)

target_link_libraries(retdec-compiler-detect-tests
    PRIVATE
        retdec::compiler-detect
        retdec::deps::gtest_main
)

set_target_properties(retdec-compiler-detect-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-compiler-detect-tests)
