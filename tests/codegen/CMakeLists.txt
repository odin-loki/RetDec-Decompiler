add_executable(retdec_codegen_tests
    codegen_test.cpp
)

target_link_libraries(retdec_codegen_tests
    PRIVATE
        retdec_codegen
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_compile_features(retdec_codegen_tests PRIVATE cxx_std_17)

add_test(NAME retdec_codegen_tests COMMAND retdec_codegen_tests)
