add_library(retdec-neural STATIC
    mock_inference.cpp
    gates.cpp
    refiner.cpp
    prompts.cpp
    decompile_hook.cpp
)

if(TARGET retdec::deps::llamacpp)
    target_sources(retdec-neural PRIVATE llama_inference.cpp)
    target_link_libraries(retdec-neural PRIVATE retdec::deps::llamacpp)
    target_compile_definitions(retdec-neural PRIVATE RETDEC_HAS_LLAMACPP)
endif()

if(RETDEC_NEURAL_OFFLINE_ONLY)
    target_compile_definitions(retdec-neural PUBLIC RETDEC_NEURAL_OFFLINE_ONLY)
endif()

add_library(retdec::neural ALIAS retdec-neural)

target_include_directories(retdec-neural
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
)

target_link_libraries(retdec-neural
    PUBLIC
        retdec::config
)

target_compile_features(retdec-neural PUBLIC cxx_std_17)

set_target_properties(retdec-neural PROPERTIES OUTPUT_NAME "retdec-neural")

install(TARGETS retdec-neural
    EXPORT retdec-targets
    ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
    LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/retdec/neural/
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec/neural
)
