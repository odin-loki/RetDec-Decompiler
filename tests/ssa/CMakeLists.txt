add_executable(retdec-ssa-tests
    ssa_test.cpp
)

target_link_libraries(retdec-ssa-tests
    PRIVATE
        retdec-ssa
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_include_directories(retdec-ssa-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(retdec-ssa-tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(retdec-ssa-tests)
