add_executable(retdec-type-inference-tests
    type_inference_test.cpp
)

target_link_libraries(retdec-type-inference-tests
    PRIVATE
        retdec-type-inference
        retdec-ssa
        retdec::deps::gtest
        retdec::deps::gtest_main
)

target_include_directories(retdec-type-inference-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(retdec-type-inference-tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(retdec-type-inference-tests)
