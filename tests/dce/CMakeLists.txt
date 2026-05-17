add_executable(retdec-dce-tests
    dce_test.cpp
)

target_link_libraries(retdec-dce-tests PRIVATE
    retdec-dce
    retdec-call-conv
    retdec-ssa
    retdec::deps::gtest_main
)

set_target_properties(retdec-dce-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-dce-tests)
