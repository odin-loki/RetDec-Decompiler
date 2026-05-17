add_executable(retdec-var-recovery-tests
    var_recovery_test.cpp
)

target_link_libraries(retdec-var-recovery-tests
    PRIVATE
        retdec-var-recovery
        retdec-ssa
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_include_directories(retdec-var-recovery-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(retdec-var-recovery-tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(retdec-var-recovery-tests)
