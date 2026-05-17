add_executable(retdec-kotlin-emitter-tests
    kotlin_emitter_test.cpp
)

target_include_directories(retdec-kotlin-emitter-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-kotlin-emitter-tests
    PRIVATE
        retdec-kotlin-emitter
        retdec-jvm-parser
        gtest
        gtest_main
)

set_target_properties(retdec-kotlin-emitter-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-kotlin-emitter-tests
         COMMAND retdec-kotlin-emitter-tests)
