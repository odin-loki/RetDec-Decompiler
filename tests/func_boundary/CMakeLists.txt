add_executable(retdec-func-boundary-tests
    func_boundary_test.cpp
)

target_link_libraries(retdec-func-boundary-tests
    PRIVATE
        retdec::func-boundary
        retdec::deps::gtest_main
)

set_target_properties(retdec-func-boundary-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-func-boundary-tests)
