add_library(retdec-neural STATIC
    mock_inference.cpp
    gates.cpp
    refiner.cpp
    prompts.cpp
    decompile_hook.cpp
    batch_refiner.cpp
    model_verify.cpp
)

# The inference backend: Forge (third_party/forge, cmake/first_party.cmake).
# RETDEC_HAS_FORGE_INFERENCE tells mock_inference.cpp not to define the null
# createForgeInference() that the dependency-free standalone check links.
#
# BUILD_INTERFACE: retdec_forge_inference is not in the retdec-targets export
# set and cannot be (it is a tree of Forge's own targets). The two archives it
# contributes to a static retdec-neural are installed below instead, and
# src/retdec/retdec-retdec-config.cmake declares them for installed consumers,
# the way cmake/tree_sitter.cmake does for tree-sitter.
target_sources(retdec-neural PRIVATE forge_inference.cpp)
target_link_libraries(retdec-neural PRIVATE $<BUILD_INTERFACE:retdec::forge_inference>)
# Not a target name in this build (that would make install(EXPORT) demand
# forge_simd_mv in the export set); the installed config declares it.
target_link_libraries(retdec-neural INTERFACE $<INSTALL_INTERFACE:retdec-forge-simd-mv>)
target_compile_definitions(retdec-neural PRIVATE RETDEC_HAS_FORGE_INFERENCE)
if(RETDEC_NEURAL_GPU_OFFLOAD)
    target_compile_definitions(retdec-neural PRIVATE RETDEC_NEURAL_GPU_OFFLOAD)
endif()
# The importers (.gguf / Hugging Face -> .fpk) read Forge's blueprint
# templates. Forge bakes in its source tree; an installed RetDec ships them in
# its support directory and forge_inference.cpp finds them relative to the
# binary when that tree is gone.
install(DIRECTORY ${RETDEC_THIRD_PARTY_DIR}/forge/import/templates/
    DESTINATION ${RETDEC_INSTALL_SUPPORT_DIR}/forge/templates
)
file(RELATIVE_PATH _RETDEC_NEURAL_FORGE_TEMPLATES_REL
    "${CMAKE_INSTALL_PREFIX}/${RETDEC_INSTALL_BIN_DIR}"
    "${CMAKE_INSTALL_PREFIX}/${RETDEC_INSTALL_SUPPORT_DIR}/forge/templates")
target_compile_definitions(retdec-neural PRIVATE
    "RETDEC_NEURAL_FORGE_TEMPLATES_REL=\"${_RETDEC_NEURAL_FORGE_TEMPLATES_REL}\""
)
install(FILES
    $<TARGET_FILE:forge_simd_mv>
    $<TARGET_FILE:forge_trust>
    DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)

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
    PRIVATE
        retdec::utils
)

if(NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL QUIET)
endif()
if(TARGET OpenSSL::Crypto)
    target_link_libraries(retdec-neural PRIVATE OpenSSL::Crypto)
    target_compile_definitions(retdec-neural PRIVATE RETDEC_NEURAL_HAS_OPENSSL)
    if(WIN32)
        # Static libcrypto needs these at the final exe link (retdec-gui).
        if(MINGW)
            target_link_libraries(retdec-neural PUBLIC crypt32 ws2_32)
        else()
            target_link_libraries(retdec-neural PUBLIC Crypt32 ws2_32)
        endif()
    endif()
endif()

target_compile_features(retdec-neural PUBLIC cxx_std_23)

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
