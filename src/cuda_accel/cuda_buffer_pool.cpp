/**
 * @file src/cuda_accel/cuda_buffer_pool.cpp
 * @brief Pooled CUDA device memory allocator.
 */
#include "retdec/cuda_accel/cuda_buffer_pool.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <vector>

#ifdef RETDEC_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace retdec::cuda_accel {

// ---------------------------------------------------------------------------
// Helpers

static std::size_t bucketFor(std::size_t bytes) noexcept {
    std::size_t bucket = 0;
    std::size_t sz     = 1;
    while (sz < bytes && bucket < CUDABufferPool::kNumBuckets - 1) {
        ++bucket;
        sz <<= 1;
    }
    return bucket;
}

static std::size_t sizeOfBucket(std::size_t bucket) noexcept {
    return std::size_t(1) << bucket;
}

// ---------------------------------------------------------------------------
// Impl

struct CUDABufferPool::Impl {
    struct Slot { void* ptr; };
    std::array<std::vector<Slot>, kNumBuckets> cache;
    std::size_t maxPerBucket;
    mutable std::mutex mu;

    explicit Impl(std::size_t maxPerBucket_) : maxPerBucket(maxPerBucket_) {}
};

// ---------------------------------------------------------------------------
// PooledBuffer

PooledBuffer::PooledBuffer(void* mem, std::size_t sz, CUDABufferPool* pool) noexcept
    : mem_(mem), size_(sz), pool_(pool) {}

PooledBuffer::PooledBuffer(PooledBuffer&& o) noexcept
    : mem_(o.mem_), size_(o.size_), pool_(o.pool_) {
    o.mem_  = nullptr;
    o.size_ = 0;
    o.pool_ = nullptr;
}

PooledBuffer& PooledBuffer::operator=(PooledBuffer&& o) noexcept {
    if (this != &o) {
        release();
        mem_  = o.mem_;
        size_ = o.size_;
        pool_ = o.pool_;
        o.mem_  = nullptr;
        o.size_ = 0;
        o.pool_ = nullptr;
    }
    return *this;
}

PooledBuffer::~PooledBuffer() { release(); }

void PooledBuffer::release() noexcept {
    if (mem_ && pool_) {
        pool_->returnToPool(mem_, size_);
    } else if (mem_) {
#ifdef RETDEC_HAS_CUDA
        cudaFree(mem_);
#endif
    }
    mem_  = nullptr;
    size_ = 0;
    pool_ = nullptr;
}

// ---------------------------------------------------------------------------
// CUDABufferPool

CUDABufferPool::CUDABufferPool(std::size_t maxCachedPerBucket)
    : impl_(std::make_unique<Impl>(maxCachedPerBucket)) {}

CUDABufferPool::~CUDABufferPool() { trimAll(); }

PooledBuffer CUDABufferPool::acquire(std::size_t minBytes) {
    std::size_t bucket = bucketFor(minBytes);
    std::size_t sz     = sizeOfBucket(bucket);

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto& slots = impl_->cache[bucket];
        if (!slots.empty()) {
            void* ptr = slots.back().ptr;
            slots.pop_back();
            return PooledBuffer(ptr, sz, this);
        }
    }

    void* ptr = nullptr;
#ifdef RETDEC_HAS_CUDA
    if (cudaMalloc(&ptr, sz) != cudaSuccess) ptr = nullptr;
#endif
    return PooledBuffer(ptr, ptr ? sz : 0, ptr ? this : nullptr);
}

void* CUDABufferPool::allocate(std::size_t bytes, int* errOut) {
    void* ptr = nullptr;
#ifdef RETDEC_HAS_CUDA
    cudaError_t e = cudaMalloc(&ptr, bytes);
    if (errOut) *errOut = static_cast<int>(e);
    if (e != cudaSuccess) return nullptr;
#else
    if (errOut) *errOut = -1;
#endif
    return ptr;
}

std::size_t CUDABufferPool::cachedCount() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::size_t total = 0;
    for (auto& v : impl_->cache) total += v.size();
    return total;
}

void CUDABufferPool::trimAll() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (auto& slots : impl_->cache) {
        for (auto& s : slots) {
#ifdef RETDEC_HAS_CUDA
            if (s.ptr) cudaFree(s.ptr);
#endif
        }
        slots.clear();
    }
}

void CUDABufferPool::returnToPool(void* mem, std::size_t size) noexcept {
    if (!mem) return;
    std::size_t bucket = bucketFor(size);
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto& slots = impl_->cache[bucket];
        if (slots.size() < impl_->maxPerBucket) {
            slots.push_back({mem});
            return;
        }
    }
#ifdef RETDEC_HAS_CUDA
    cudaFree(mem);
#endif
}

} // namespace retdec::cuda_accel
