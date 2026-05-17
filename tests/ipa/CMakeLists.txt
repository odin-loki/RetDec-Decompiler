add_executable(retdec-ipa-tests
    ipa_test.cpp
)

target_link_libraries(retdec-ipa-tests PRIVATE
    retdec-ipa
    retdec-call-conv
    retdec-ssa
    retdec::deps::gtest_main
)

set_target_properties(retdec-ipa-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-ipa-tests)
