add_executable(retdec-call-conv-tests
    call_conv_test.cpp
)

target_link_libraries(retdec-call-conv-tests PRIVATE
    retdec-call-conv
    retdec-ssa
    retdec::deps::gtest_main
)

set_target_properties(retdec-call-conv-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-call-conv-tests)
