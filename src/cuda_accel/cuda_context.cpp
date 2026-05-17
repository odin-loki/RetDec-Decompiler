/**
 * @file src/cuda_accel/cuda_context.cpp
 * @brief CUDAContext implementation.
 */
#include "retdec/cuda_accel/cuda_context.h"
#include "retdec/cuda_accel/cuda_error.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef RETDEC_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace retdec::cuda_accel {

CUDAContext::CUDAContext() = default;

CUDAContext::~CUDAContext() {
    reset();
}

bool CUDAContext::initialize() {
    if (ready_) return true;

#ifndef RETDEC_HAS_CUDA
    lastError_ = "CUDA support not compiled in";
    return false;
#else
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess || count == 0) {
        lastError_ = "No CUDA devices found";
        return false;
    }

    devices_.clear();
    devices_.reserve(static_cast<std::size_t>(count));

    int bestId    = 0;
    int bestScore = -1;

    for (int i = 0; i < count; ++i) {
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, i) != cudaSuccess) continue;

        CUDADeviceInfo di;
        di.id                  = i;
        di.name                = prop.name;
        di.globalMemBytes      = prop.totalGlobalMem;
        di.sharedMemPerBlock   = prop.sharedMemPerBlock;
        di.maxThreadsPerBlock  = prop.maxThreadsPerBlock;
        di.multiProcessorCount = prop.multiProcessorCount;
        di.computeMajor        = prop.major;
        di.computeMinor        = prop.minor;
        di.available           = true;

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d.%d", prop.major, prop.minor);
        di.driverVersion = buf;

        devices_.push_back(di);

        int sc = di.score();
        if (sc > bestScore) { bestScore = sc; bestId = i; }
    }

    if (devices_.empty()) {
        lastError_ = "No usable CUDA devices";
        return false;
    }

    err = cudaSetDevice(bestId);
    if (err != cudaSuccess) {
        lastError_ = std::string("cudaSetDevice: ") + cudaGetErrorString(err);
        return false;
    }

    err = cudaStreamCreate(&stream_);
    if (err != cudaSuccess) {
        lastError_ = std::string("cudaStreamCreate: ") + cudaGetErrorString(err);
        return false;
    }

    devId_   = bestId;
    primary_ = devices_[static_cast<std::size_t>(bestId)];
    ready_   = true;
    return true;
#endif
}

void CUDAContext::reset() {
#ifdef RETDEC_HAS_CUDA
    if (stream_) {
        cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }
#endif
    ready_ = false;
    devId_ = -1;
    primary_ = {};
    devices_.clear();
}

void* CUDAContext::createBuffer(std::size_t bytes, int* errOut) {
#ifdef RETDEC_HAS_CUDA
    void* ptr = nullptr;
    cudaError_t e = cudaMalloc(&ptr, bytes);
    if (errOut) *errOut = static_cast<int>(e);
    if (e != cudaSuccess) return nullptr;
    return ptr;
#else
    if (errOut) *errOut = -1;
    return nullptr;
#endif
}

int CUDAContext::writeBuffer(void* devPtr, const void* src, std::size_t bytes) {
#ifdef RETDEC_HAS_CUDA
    return static_cast<int>(cudaMemcpyAsync(devPtr, src, bytes,
                                             cudaMemcpyHostToDevice, stream_));
#else
    return -1;
#endif
}

int CUDAContext::readBuffer(void* devPtr, void* dst, std::size_t bytes) {
#ifdef RETDEC_HAS_CUDA
    return static_cast<int>(cudaMemcpyAsync(dst, devPtr, bytes,
                                             cudaMemcpyDeviceToHost, stream_));
#else
    return -1;
#endif
}

void CUDAContext::freeBuffer(void* devPtr) {
#ifdef RETDEC_HAS_CUDA
    if (devPtr) cudaFree(devPtr);
#else
    (void)devPtr;
#endif
}

int CUDAContext::synchronize() {
#ifdef RETDEC_HAS_CUDA
    return static_cast<int>(cudaStreamSynchronize(stream_));
#else
    return 0;
#endif
}

} // namespace retdec::cuda_accel
