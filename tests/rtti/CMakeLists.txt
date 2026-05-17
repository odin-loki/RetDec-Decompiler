##
## tests/rtti/CMakeLists.txt
##

add_executable(retdec-rtti-tests
    rtti_test.cpp
    rtti_extra_test.cpp
)

target_link_libraries(retdec-rtti-tests
    PRIVATE
        retdec::rtti
        retdec::deps::gtest_main
)

set_target_properties(retdec-rtti-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

gtest_discover_tests(retdec-rtti-tests)
