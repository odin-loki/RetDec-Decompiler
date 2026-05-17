/**
 * @file include/retdec/cuda_accel/cuda_error.h
 * @brief CUDA error helpers — replaces ocl_error.h.
 */
#pragma once

#include <stdexcept>
#include <string>

#ifdef RETDEC_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace retdec::cuda_accel {

#ifdef RETDEC_HAS_CUDA
inline std::string cudaErrorString(cudaError_t err) noexcept {
    return cudaGetErrorString(err);
}
#else
inline std::string cudaErrorString(int err) noexcept {
    return "CUDA not available (err=" + std::to_string(err) + ")";
}
#endif

class CUDAException : public std::runtime_error {
public:
    explicit CUDAException(const std::string& msg, int code = 0,
                           const char* file = nullptr, int line = 0)
        : std::runtime_error(
              std::string("[CUDA] ") + msg +
              (file ? (" at " + std::string(file) + ":" + std::to_string(line)) : "") +
              " (code=" + std::to_string(code) + ")")
        , code_(code) {}

    int code() const noexcept { return code_; }

private:
    int code_;
};

} // namespace retdec::cuda_accel

#ifdef RETDEC_HAS_CUDA
#define RETDEC_CUDA_CHECK(expr, msg) \
    do { \
        cudaError_t _e = (expr); \
        if (_e != cudaSuccess) \
            throw ::retdec::cuda_accel::CUDAException( \
                (msg), static_cast<int>(_e), __FILE__, __LINE__); \
    } while (0)
#else
#define RETDEC_CUDA_CHECK(expr, msg) ((void)(msg))
#endif
