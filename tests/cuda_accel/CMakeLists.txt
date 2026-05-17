
add_executable(retdec-cuda-accel-tests
    cuda_context_test.cpp
    cuda_buffer_pool_test.cpp
    cuda_profiler_test.cpp
    cuda_disassembler_test.cpp
    cuda_steensgaard_test.cpp
    cuda_type_inferencer_test.cpp
    cuda_semantic_hasher_test.cpp
    cuda_egraph_simplifier_test.cpp
)

target_link_libraries(retdec-cuda-accel-tests
    PRIVATE
        retdec::cuda-accel
        retdec::deps::gtest_main
)

# Match retdec-cuda-accel: avoid cxx_std_23 on CUDA-linked targets (CMake -> CUDA23).
set_target_properties(retdec-cuda-accel-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

include(GoogleTest)
gtest_discover_tests(retdec-cuda-accel-tests)
