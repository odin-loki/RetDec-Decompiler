add_library(retdec-neural STATIC
    mock_inference.cpp
    gates.cpp
    refiner.cpp
    prompts.cpp
    decompile_hook.cpp
    batch_refiner.cpp
    model_verify.cpp
)

if(TARGET retdec::deps::llamacpp)
    target_sources(retdec-neural PRIVATE llama_inference.cpp)
    target_link_libraries(retdec-neural PRIVATE retdec::deps::llamacpp)
    target_compile_definitions(retdec-neural PRIVATE RETDEC_HAS_LLAMACPP)
    if(RETDEC_NEURAL_GPU_OFFLOAD)
        target_compile_definitions(retdec-neural PRIVATE RETDEC_NEURAL_GPU_OFFLOAD)
    endif()
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

include(${PROJECT_SOURCE_DIR}/cmake/tree_sitter.cmake)
target_link_libraries(retdec-neural PUBLIC retdec::deps::tree-sitter retdec::deps::tree-sitter-c)
target_compile_definitions(retdec-neural PRIVATE RETDEC_HAS_TREE_SITTER)

target_link_libraries(retdec-neural
    PUBLIC
        retdec::config
)

if(NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL QUIET)
endif()
if(TARGET OpenSSL::Crypto)
    target_link_libraries(retdec-neural PRIVATE OpenSSL::Crypto)
    target_compile_definitions(retdec-neural PRIVATE RETDEC_NEURAL_HAS_OPENSSL)
endif()

target_compile_features(retdec-neural PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(retdec-neural PRIVATE -Werror)
endif()

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/support/models.json" _RETDEC_NEURAL_DEFAULT_MODELS_JSON)
target_compile_definitions(retdec-neural PRIVATE
    "RETDEC_NEURAL_DEFAULT_MODELS_JSON=\"${_RETDEC_NEURAL_DEFAULT_MODELS_JSON}\""
)

set_target_properties(retdec-neural PROPERTIES OUTPUT_NAME "retdec-neural")

install(FILES
    ${PROJECT_SOURCE_DIR}/support/models.json
    ${PROJECT_SOURCE_DIR}/support/models.md
    DESTINATION ${RETDEC_INSTALL_SUPPORT_DIR}
)

install(TARGETS retdec-neural
    EXPORT retdec-targets
    ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
    LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/retdec/neural/
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec/neural
)
