add_executable(retdec-cfg-structure-tests
    cfg_structure_test.cpp
)

target_link_libraries(retdec-cfg-structure-tests PRIVATE
    retdec-cfg-structure
    retdec-ssa
    retdec::deps::gtest_main
)

set_target_properties(retdec-cfg-structure-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-cfg-structure-tests)
