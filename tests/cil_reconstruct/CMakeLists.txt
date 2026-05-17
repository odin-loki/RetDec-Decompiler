add_executable(retdec-cil-reconstruct-tests
    cil_reconstruct_test.cpp
)

target_include_directories(retdec-cil-reconstruct-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-cil-reconstruct-tests
    PRIVATE
        retdec-cil-reconstruct
        retdec-bc-module
        gtest
        gtest_main
)

set_target_properties(retdec-cil-reconstruct-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME cil-reconstruct-tests COMMAND retdec-cil-reconstruct-tests)
