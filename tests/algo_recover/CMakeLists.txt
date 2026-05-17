add_executable(retdec_algo_recover_tests
    algo_recover_test.cpp
)

target_link_libraries(retdec_algo_recover_tests
    PRIVATE
        retdec_algo_recover
        retdec_ssa
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec_algo_recover_tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec_algo_recover_tests
         COMMAND retdec_algo_recover_tests)
