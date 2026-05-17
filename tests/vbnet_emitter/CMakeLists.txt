add_executable(retdec-vbnet-emitter-tests
    vbnet_emitter_test.cpp
)

target_include_directories(retdec-vbnet-emitter-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-vbnet-emitter-tests
    PRIVATE
        retdec-vbnet-emitter
        retdec-bc-module
        gtest
        gtest_main
)

set_target_properties(retdec-vbnet-emitter-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME vbnet-emitter-tests COMMAND retdec-vbnet-emitter-tests)
