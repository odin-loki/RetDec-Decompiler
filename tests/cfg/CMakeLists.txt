add_executable(retdec-cfg-tests
    cfg_test.cpp
)

target_link_libraries(retdec-cfg-tests
    PRIVATE
        retdec::cfg
        retdec::deps::gtest_main
)

set_target_properties(retdec-cfg-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-cfg-tests)
