/**
 * @file include/retdec/cuda_accel/cuda_buffer_pool.h
 * @brief Pooled CUDA device memory allocator — replaces OCLBufferPool.
 *
 * Buffers are organised in power-of-2 size buckets and returned to the pool
 * on destruction of the PooledBuffer RAII handle.
 */
#pragma once

#include <cstddef>
#include <memory>

namespace retdec::cuda_accel {

class CUDABufferPool;

/// RAII handle to a pooled CUDA device buffer.
class PooledBuffer {
public:
    PooledBuffer() noexcept = default;
    PooledBuffer(void* mem, std::size_t sz, CUDABufferPool* pool) noexcept;
    ~PooledBuffer();

    PooledBuffer(PooledBuffer&& o) noexcept;
    PooledBuffer& operator=(PooledBuffer&& o) noexcept;

    PooledBuffer(const PooledBuffer&)            = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;

    void*       mem()   const noexcept { return mem_; }
    std::size_t size()  const noexcept { return size_; }
    bool        valid() const noexcept { return mem_ != nullptr; }

private:
    void         release() noexcept;

    void*           mem_{nullptr};
    std::size_t     size_{0};
    CUDABufferPool* pool_{nullptr};
};

class CUDABufferPool {
public:
    static constexpr std::size_t kNumBuckets = 32;

    explicit CUDABufferPool(std::size_t maxCachedPerBucket = 8);
    ~CUDABufferPool();

    CUDABufferPool(const CUDABufferPool&)            = delete;
    CUDABufferPool& operator=(const CUDABufferPool&) = delete;

    /// Acquire a buffer of at least @p minBytes. May reuse a cached buffer.
    PooledBuffer acquire(std::size_t minBytes);

    /// Allocate a raw device buffer (not pooled).
    void* allocate(std::size_t bytes, int* errOut = nullptr);

    std::size_t cachedCount() const;
    void        trimAll();

    /// Called by PooledBuffer::~PooledBuffer to return memory to the pool.
    void returnToPool(void* mem, std::size_t size) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace retdec::cuda_accel
