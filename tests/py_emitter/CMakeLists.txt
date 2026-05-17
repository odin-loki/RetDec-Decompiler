add_executable(retdec-py-emitter-tests
    py_emitter_test.cpp
)

target_include_directories(retdec-py-emitter-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-py-emitter-tests
    PRIVATE
        retdec-py-emitter
        retdec-py-reconstruct
        gtest
        gtest_main
)

set_target_properties(retdec-py-emitter-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME py-emitter-tests COMMAND retdec-py-emitter-tests)
