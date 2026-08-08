add_executable(retdec-neural-tests mock_test.cpp)

target_link_libraries(retdec-neural-tests
    PRIVATE
        retdec::neural
        retdec::deps::gtest
        retdec::deps::gtest_main
)

add_test(NAME retdec-neural-tests COMMAND retdec-neural-tests)
set_tests_properties(retdec-neural-tests PROPERTIES LABELS "unit;neural")
