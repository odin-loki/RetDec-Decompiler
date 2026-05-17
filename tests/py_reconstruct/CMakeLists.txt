add_executable(retdec-py-reconstruct-tests
    py_reconstruct_test.cpp
)

target_include_directories(retdec-py-reconstruct-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-py-reconstruct-tests
    PRIVATE
        retdec-py-reconstruct
        retdec-pyc-parser
        retdec-bc-module
        gtest
        gtest_main
)

set_target_properties(retdec-py-reconstruct-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME py-reconstruct-tests COMMAND retdec-py-reconstruct-tests)
