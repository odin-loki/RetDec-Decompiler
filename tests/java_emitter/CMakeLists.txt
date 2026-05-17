add_executable(retdec-java-emitter-tests
    java_emitter_test.cpp
)

target_link_libraries(retdec-java-emitter-tests
    PRIVATE retdec-java-emitter
            retdec-jvm-parser
            retdec::deps::gtest
            retdec::deps::gtest_main
)

set_target_properties(retdec-java-emitter-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-java-emitter-tests
    COMMAND retdec-java-emitter-tests
)
