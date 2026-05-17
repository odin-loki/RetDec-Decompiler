cmake_minimum_required(VERSION 3.21)

# ── CUDA GPU backend ──────────────────────────────────────────────────────────
# Respect RETDEC_ENABLE_CUDA_ACCEL (e.g. -NoCuda / CPU-only Windows configure) so we
# do not call enable_language(CUDA) when the toolkit is present but unusable with MSVC.
find_package(CUDAToolkit QUIET)

if(RETDEC_ENABLE_CUDA_ACCEL AND CUDAToolkit_FOUND)
    enable_language(CUDA)
    message(STATUS "Qwen3: CUDA ${CUDAToolkit_VERSION} — GPU inference enabled (RTX/GTX/A100)")
    set(RETDEC_HAS_CUDA ON)
elseif(NOT RETDEC_ENABLE_CUDA_ACCEL)
    message(STATUS "Qwen3: RETDEC_ENABLE_CUDA_ACCEL=OFF — CPU-only inference")
    set(RETDEC_HAS_CUDA OFF)
else()
    message(STATUS "Qwen3: CUDA not found — CPU-only inference")
    set(RETDEC_HAS_CUDA OFF)
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
set(QWEN3_SOURCES
    qwen3_config.cpp
    qwen3_trace.cpp
    qwen3_tokenizer.cpp
    qwen3_weights.cpp
    qwen3_ops.cpp
    qwen3_attention.cpp
    qwen3_moe.cpp
    qwen3_pipeline.cpp
    qwen3_model.cpp
    qwen3_sampler.cpp
)

if(RETDEC_HAS_CUDA)
    list(APPEND QWEN3_SOURCES qwen3_cuda.cu)
else()
    # Stub compiled as plain C++ — no CUDA compiler needed
    list(APPEND QWEN3_SOURCES qwen3_cuda_stub.cpp)
endif()

add_library(retdec-qwen3 STATIC ${QWEN3_SOURCES})

target_include_directories(retdec-qwen3
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(retdec-qwen3 PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED YES
)

# ── CUDA configuration ────────────────────────────────────────────────────────
if(RETDEC_HAS_CUDA)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
        set_target_properties(retdec-qwen3 PROPERTIES CUDA_ARCHITECTURES native)
    else()
        set_target_properties(retdec-qwen3 PROPERTIES CUDA_ARCHITECTURES "75;80;86;89")
    endif()

    set_target_properties(retdec-qwen3 PROPERTIES
        CUDA_STANDARD          17
        CUDA_STANDARD_REQUIRED YES
    )

    target_compile_definitions(retdec-qwen3 PUBLIC RETDEC_HAS_CUDA)
    target_link_libraries(retdec-qwen3 PUBLIC CUDA::cudart)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS retdec-qwen3 ARCHIVE DESTINATION lib)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/retdec/qwen3
    DESTINATION include/retdec
)
