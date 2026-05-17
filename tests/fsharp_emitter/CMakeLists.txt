add_executable(retdec-fsharp-emitter-tests
    fsharp_emitter_test.cpp
)

target_include_directories(retdec-fsharp-emitter-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-fsharp-emitter-tests
    PRIVATE
        retdec-fsharp-emitter
        retdec-bc-module
        gtest
        gtest_main
)

set_target_properties(retdec-fsharp-emitter-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME fsharp-emitter-tests COMMAND retdec-fsharp-emitter-tests)
