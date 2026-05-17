add_executable(retdec-csharp-emitter-tests
    cs_emitter_test.cpp
)

target_include_directories(retdec-csharp-emitter-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-csharp-emitter-tests
    PRIVATE
        retdec-csharp-emitter
        retdec-cil-reconstruct
        retdec-bc-module
        gtest
        gtest_main
)

set_target_properties(retdec-csharp-emitter-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME csharp-emitter-tests COMMAND retdec-csharp-emitter-tests)
