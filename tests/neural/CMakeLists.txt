add_executable(retdec-neural-tests mock_test.cpp)

target_link_libraries(retdec-neural-tests
    PRIVATE
        retdec::neural
        retdec::deps::gtest
        retdec::deps::gtest_main
)

add_test(NAME retdec-neural-tests COMMAND retdec-neural-tests)
set_tests_properties(retdec-neural-tests PROPERTIES LABELS "unit;neural")

# The Forge backend end to end on Forge's golden tiny model
# (tests/neural/data/tiny_f32.fpk, 459 KB, random weights).
add_executable(retdec-neural-forge-tests forge_inference_test.cpp)
target_link_libraries(retdec-neural-forge-tests
    PRIVATE
        retdec::neural
        retdec::deps::rapidjson
        retdec::deps::gtest
        retdec::deps::gtest_main
)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/data" _RETDEC_NEURAL_TEST_DATA_DIR)
target_compile_definitions(retdec-neural-forge-tests PRIVATE
    "RETDEC_NEURAL_TEST_DATA_DIR=\"${_RETDEC_NEURAL_TEST_DATA_DIR}\""
)
add_test(NAME retdec-neural-forge-tests COMMAND retdec-neural-forge-tests)
set_tests_properties(retdec-neural-forge-tests PROPERTIES LABELS "unit;neural")

# The Hugging Face import path needs a model directory, which Forge's own
# generator writes (python3 + numpy + safetensors). Without those modules the
# fixture is not added and the import test skips itself.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import numpy, safetensors"
        RESULT_VARIABLE _retdec_neural_py_ok
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_retdec_neural_py_ok EQUAL 0)
        set(_hf_dir "${CMAKE_CURRENT_BINARY_DIR}/tiny_hf")
        add_test(NAME retdec-neural-forge-tiny-hf
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/make_tiny_hf.py
                ${RETDEC_THIRD_PARTY_DIR}/forge
                ${_hf_dir}
        )
        set_tests_properties(retdec-neural-forge-tiny-hf PROPERTIES
            FIXTURES_SETUP retdec_neural_tiny_hf
            LABELS "unit;neural"
        )
        set_tests_properties(retdec-neural-forge-tests PROPERTIES
            FIXTURES_REQUIRED retdec_neural_tiny_hf
            ENVIRONMENT "RETDEC_NEURAL_TEST_HF_DIR=${_hf_dir}"
        )
    endif()
endif()
