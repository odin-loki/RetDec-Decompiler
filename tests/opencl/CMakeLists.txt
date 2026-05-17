
find_package(OpenCL QUIET)

if(NOT TARGET OpenCL::OpenCL)
    message(STATUS "OpenCL not found – skipping opencl tests")
    return()
endif()

add_executable(retdec-opencl-tests
    ocl_profiler_test.cpp
    ocl_buffer_pool_test.cpp
    ocl_context_test.cpp
    ocl_disassembler_test.cpp
    ocl_type_inferencer_test.cpp
    ocl_steensgaard_test.cpp
    ocl_semantic_hasher_test.cpp
    ocl_egraph_simplifier_test.cpp
)

target_link_libraries(retdec-opencl-tests
    PRIVATE
        retdec::opencl
        retdec::deps::gtest_main
)

target_compile_features(retdec-opencl-tests PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(retdec-opencl-tests)
