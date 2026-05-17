add_executable(retdec-loader-sim-tests
    loader_sim_test.cpp
)

target_link_libraries(retdec-loader-sim-tests
    PRIVATE
        retdec::loader-sim
        retdec::deps::gtest_main
)

set_target_properties(retdec-loader-sim-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-loader-sim-tests)
