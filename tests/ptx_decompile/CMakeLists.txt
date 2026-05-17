add_executable(retdec-ptx-decompile-tests
    cuda_host_recover_test.cpp
    ptx_lifter_test.cpp
)

target_include_directories(retdec-ptx-decompile-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-ptx-decompile-tests
    PRIVATE
        retdec-ptx-decompile
        retdec::deps::gtest
        retdec::deps::gtest_main
)

set_target_properties(retdec-ptx-decompile-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-ptx-decompile-tests
    COMMAND retdec-ptx-decompile-tests
)
