/**
 * @file include/retdec/opencl/ocl_buffer_pool.h
 * @brief Power-of-2 bucket buffer pool for OpenCL device memory.
 *
 * Maintains per-size-class free lists so that small allocations don't
 * round-trip through clCreateBuffer / clReleaseMemObject on every use.
 *
 * Usage:
 *   OCLBufferPool pool(ctx.clContext(), ctx.clDevice());
 *
 *   auto guard = pool.acquire(1024);   // returns a PooledBuffer RAII guard
 *   cl_mem buf = guard.mem();          // valid cl_mem
 *   // …set kernel args, enqueue…
 *   // guard destructor returns buffer to pool automatically
 */

#ifndef RETDEC_OPENCL_OCL_BUFFER_POOL_H
#define RETDEC_OPENCL_OCL_BUFFER_POOL_H

#include <CL/cl.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace retdec {
namespace opencl {

// ─── RAII guard returned by OCLBufferPool::acquire() ─────────────────────────
class OCLBufferPool;

class PooledBuffer {
public:
    PooledBuffer() = default;
    ~PooledBuffer();

    PooledBuffer(PooledBuffer&&) noexcept;
    PooledBuffer& operator=(PooledBuffer&&) noexcept;

    PooledBuffer(const PooledBuffer&)            = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;

    cl_mem  mem()      const noexcept { return _mem; }
    std::size_t size() const noexcept { return _size; }
    bool    valid()    const noexcept { return _mem != nullptr; }

private:
    friend class OCLBufferPool;
    PooledBuffer(cl_mem mem, std::size_t size,
                 std::size_t bucketIdx, OCLBufferPool* owner) noexcept;

    cl_mem         _mem       = nullptr;
    std::size_t    _size      = 0;
    std::size_t    _bucketIdx = 0;
    OCLBufferPool* _owner     = nullptr;
};

// ─── OCLBufferPool ────────────────────────────────────────────────────────────
class OCLBufferPool {
public:
    /// @param maxCachedPerBucket Maximum free cl_mem objects to keep per size class.
    explicit OCLBufferPool(cl_context ctx, cl_device_id dev,
                           std::size_t maxCachedPerBucket = 8);
    ~OCLBufferPool();

    OCLBufferPool(const OCLBufferPool&)            = delete;
    OCLBufferPool& operator=(const OCLBufferPool&) = delete;

    /// Acquire a buffer of at least @p minBytes (rounded up to next power-of-2).
    /// Returns an invalid PooledBuffer on allocation failure.
    PooledBuffer acquire(std::size_t minBytes);

    /// Direct allocation (no pool). Caller must clReleaseMemObject.
    cl_mem allocate(std::size_t bytes, cl_int* errOut = nullptr);

    /// Return cached buffer count across all buckets (for diagnostics).
    std::size_t cachedCount() const;

    /// Release all cached (free) buffers back to the OpenCL runtime.
    void trimAll();

private:
    friend class PooledBuffer;
    void returnToPool(cl_mem mem, std::size_t bucketIdx);

    static std::size_t bucketIndex(std::size_t bytes) noexcept;
    static std::size_t bucketSize(std::size_t idx)    noexcept;

    cl_context   _ctx = nullptr;
    cl_device_id _dev = nullptr;
    std::size_t  _maxCached;

    static constexpr std::size_t kNumBuckets = 32; // 1 byte … 2 GiB

    mutable std::mutex            _mtx;
    std::vector<cl_mem>           _free[kNumBuckets];
};

} // namespace opencl
} // namespace retdec

#endif // RETDEC_OPENCL_OCL_BUFFER_POOL_H
