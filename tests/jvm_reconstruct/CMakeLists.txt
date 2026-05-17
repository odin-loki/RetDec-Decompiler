add_executable(retdec-jvm-reconstruct-tests
    jvm_reconstruct_test.cpp
)

target_link_libraries(retdec-jvm-reconstruct-tests
    PRIVATE retdec-jvm-reconstruct
            retdec-jvm-parser
            retdec::deps::gtest
            retdec::deps::gtest_main
)

set_target_properties(retdec-jvm-reconstruct-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-jvm-reconstruct-tests
    COMMAND retdec-jvm-reconstruct-tests
)
