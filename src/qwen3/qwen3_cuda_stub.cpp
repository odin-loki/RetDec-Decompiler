/**
 * @file src/qwen3/qwen3_cuda_stub.cpp
 * @brief No-op stub for Qwen3CUDA when CUDA is not available.
 *
 * Compiled instead of qwen3_cuda.cu on non-CUDA builds so the rest of the
 * codebase compiles unchanged — all methods return false / do nothing.
 *
 * @copyright (c) 2024 Odin Loch Trading as Imortek
 */

#include "retdec/qwen3/qwen3_cuda.h"
#include "retdec/qwen3/qwen3_ops.h"

// RETDEC_HAS_CUDA is not defined — use the no-op branch from the .cu file
// by duplicating the stub here (avoids requiring nvcc).

namespace retdec::qwen3 {

Qwen3CUDA::Qwen3CUDA()  = default;
Qwen3CUDA::~Qwen3CUDA() = default;

bool Qwen3CUDA::init(float, int)                              { return false; }
void Qwen3CUDA::setGpuFraction(float)                         {}
void Qwen3CUDA::shutdown()                                    {}
bool Qwen3CUDA::uploadWeight(const void*, const uint8_t*, std::size_t) {
    lastError_ = "CUDA not available";
    return false;
}
void Qwen3CUDA::releaseWeight(const void*)                    {}
void Qwen3CUDA::releaseAllWeights()                           {}
bool Qwen3CUDA::gemv(const uint8_t* wData, const void*,
                     GgufDtype dtype, int rows, int cols,
                     const float* x, float* y) {
    ops::gemv(wData, dtype, rows, cols, x, y);
    return false;
}
std::vector<CudaDeviceInfo> Qwen3CUDA::enumDevices() { return {}; }

} // namespace retdec::qwen3
