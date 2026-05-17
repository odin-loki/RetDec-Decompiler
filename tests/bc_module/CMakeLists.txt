add_executable(retdec-bc-module-tests
    bc_module_test.cpp
)

target_link_libraries(retdec-bc-module-tests
    PRIVATE
        retdec-bc-module
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec-bc-module-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME retdec-bc-module-tests COMMAND retdec-bc-module-tests)
