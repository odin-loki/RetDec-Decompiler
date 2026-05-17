add_executable(retdec-qwen3-tests
    qwen3_test.cpp
    qwen3_attention_test.cpp
    qwen3_moe_test.cpp
    qwen3_pipeline_test.cpp
    qwen3_cuda_test.cpp
)

target_link_libraries(retdec-qwen3-tests
    PRIVATE
        retdec-qwen3
        retdec::deps::gtest_main
)

set_target_properties(retdec-qwen3-tests PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME retdec-qwen3-tests COMMAND retdec-qwen3-tests)
