add_executable(retdec_sort_detect_tests
    sort_detect_test.cpp
)

target_link_libraries(retdec_sort_detect_tests
    PRIVATE
        retdec_sort_detect
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_compile_features(retdec_sort_detect_tests PRIVATE cxx_std_17)

add_test(NAME retdec_sort_detect_tests COMMAND retdec_sort_detect_tests)
