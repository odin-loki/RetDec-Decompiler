add_executable(retdec_container_detect_tests
    container_detect_test.cpp
)

target_link_libraries(retdec_container_detect_tests
    PRIVATE
        retdec_container_detect
        retdec_ssa
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec_container_detect_tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec_container_detect_tests
         COMMAND retdec_container_detect_tests)
