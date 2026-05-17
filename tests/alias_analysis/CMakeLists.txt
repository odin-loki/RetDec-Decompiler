add_executable(retdec-alias-analysis-tests
    alias_test.cpp
)

target_link_libraries(retdec-alias-analysis-tests
    PRIVATE
        retdec-alias-analysis
        retdec-ssa
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_include_directories(retdec-alias-analysis-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(retdec-alias-analysis-tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(retdec-alias-analysis-tests)
