
find_package(OpenCL REQUIRED)

# Make our cmake modules available.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")
include(EmbedOpenCLKernels)

add_library(retdec-opencl STATIC
    ocl_context.cpp
    ocl_disk_cache.cpp
    ocl_profiler.cpp
    ocl_buffer_pool.cpp
    ocl_disassembler.cpp
    ocl_type_inferencer.cpp
    ocl_steensgaard.cpp
    ocl_semantic_hasher.cpp
    ocl_egraph_simplifier.cpp
    parallel_disasm_sources.cpp
    additional_kernel_sources.cpp
)
add_library(retdec::opencl ALIAS retdec-opencl)

# Embed all .cl kernel files as C++ string literals at build time.
retdec_embed_opencl_kernels(
    TARGET  retdec-opencl
    SOURCES
        kernels/parallel_disasm.cl
        kernels/semantic_hash.cl
        kernels/egraph_simplify.cl
        kernels/type_propagation.cl
        kernels/steensgaard.cl
)

target_compile_features(retdec-opencl PUBLIC cxx_std_23)

target_include_directories(retdec-opencl
    PUBLIC
        $<BUILD_INTERFACE:${RETDEC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${RETDEC_INSTALL_INCLUDE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(retdec-opencl
    PUBLIC
        OpenCL::OpenCL
)

set_target_properties(retdec-opencl
    PROPERTIES
        OUTPUT_NAME "retdec-opencl"
)

install(
    DIRECTORY ${RETDEC_INCLUDE_DIR}/retdec/opencl
    DESTINATION ${RETDEC_INSTALL_INCLUDE_DIR}/retdec
)

install(TARGETS retdec-opencl
    EXPORT retdec-opencl-targets
    ARCHIVE DESTINATION ${RETDEC_INSTALL_LIB_DIR}
    LIBRARY DESTINATION ${RETDEC_INSTALL_LIB_DIR}
)

install(EXPORT retdec-opencl-targets
    FILE "retdec-opencl-targets.cmake"
    NAMESPACE retdec::
    DESTINATION ${RETDEC_INSTALL_CMAKE_DIR}
)

configure_file(
    "retdec-opencl-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/retdec-opencl-config.cmake"
    @ONLY
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/retdec-opencl-config.cmake"
    DESTINATION
        "${RETDEC_INSTALL_CMAKE_DIR}"
)
