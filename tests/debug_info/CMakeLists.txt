##
## tests/debug_info/CMakeLists.txt
##

add_executable(retdec-debug-info-tests
    debug_info_test.cpp
)

target_link_libraries(retdec-debug-info-tests
    PRIVATE
        retdec::debug-info
        retdec::deps::gtest_main
)

set_target_properties(retdec-debug-info-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

gtest_discover_tests(retdec-debug-info-tests)
