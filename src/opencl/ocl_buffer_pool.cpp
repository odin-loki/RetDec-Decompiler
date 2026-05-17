/**
 * @file src/opencl/ocl_buffer_pool.cpp
 * @brief Power-of-2 bucket buffer pool implementation.
 *
 * Each bucket i holds cl_mem objects of exactly 2^i bytes.
 * acquire(n) rounds n up to the next power of 2, checks the corresponding
 * free list, and creates a new buffer only if the list is empty.
 * PooledBuffer's destructor returns the buffer to the pool instead of
 * releasing it back to the OpenCL runtime.
 */

#include "retdec/opencl/ocl_buffer_pool.h"

#include <bit>
#include <cassert>
#include <stdexcept>

namespace retdec {
namespace opencl {

// ─── PooledBuffer ─────────────────────────────────────────────────────────────

PooledBuffer::PooledBuffer(cl_mem mem, std::size_t size,
                           std::size_t bucketIdx, OCLBufferPool* owner) noexcept
    : _mem(mem), _size(size), _bucketIdx(bucketIdx), _owner(owner)
{}

PooledBuffer::PooledBuffer(PooledBuffer&& o) noexcept
    : _mem(o._mem), _size(o._size), _bucketIdx(o._bucketIdx), _owner(o._owner)
{
    o._mem   = nullptr;
    o._owner = nullptr;
}

PooledBuffer& PooledBuffer::operator=(PooledBuffer&& o) noexcept
{
    if (this != &o) {
        // Return our current buffer to pool (or release if pool is gone).
        if (_mem) {
            if (_owner) {
                _owner->returnToPool(_mem, _bucketIdx);
            } else {
                clReleaseMemObject(_mem);
            }
        }
        _mem       = o._mem;
        _size      = o._size;
        _bucketIdx = o._bucketIdx;
        _owner     = o._owner;
        o._mem     = nullptr;
        o._owner   = nullptr;
    }
    return *this;
}

PooledBuffer::~PooledBuffer()
{
    if (!_mem) { return; }
    if (_owner) {
        _owner->returnToPool(_mem, _bucketIdx);
    } else {
        clReleaseMemObject(_mem);
    }
}

// ─── OCLBufferPool ────────────────────────────────────────────────────────────

OCLBufferPool::OCLBufferPool(cl_context ctx, cl_device_id dev,
                             std::size_t maxCachedPerBucket)
    : _ctx(ctx), _dev(dev), _maxCached(maxCachedPerBucket)
{}

OCLBufferPool::~OCLBufferPool()
{
    trimAll();
}

// ── Internal: index → size and size → index ──────────────────────────────────

std::size_t OCLBufferPool::bucketIndex(std::size_t bytes) noexcept
{
    if (bytes == 0) { bytes = 1; }
    // Smallest power-of-2 >= bytes.
    if ((bytes & (bytes - 1)) == 0) {
        return static_cast<std::size_t>(std::countr_zero(bytes));
    }
    // Round up to next power of 2.
    std::size_t idx = (sizeof(bytes) * 8) - static_cast<std::size_t>(std::countl_zero(bytes));
    return idx < kNumBuckets ? idx : kNumBuckets - 1;
}

std::size_t OCLBufferPool::bucketSize(std::size_t idx) noexcept
{
    return std::size_t(1) << idx;
}

// ── acquire ───────────────────────────────────────────────────────────────────

PooledBuffer OCLBufferPool::acquire(std::size_t minBytes)
{
    if (minBytes == 0) { minBytes = 1; }
    const std::size_t idx  = bucketIndex(minBytes);
    const std::size_t size = bucketSize(idx);

    {
        std::lock_guard<std::mutex> lock(_mtx);
        auto& freeList = _free[idx];
        if (!freeList.empty()) {
            cl_mem m = freeList.back();
            freeList.pop_back();
            return PooledBuffer(m, size, idx, this);
        }
    }

    // Nothing cached — allocate fresh.
    cl_int err = CL_SUCCESS;
    cl_mem m = clCreateBuffer(_ctx, CL_MEM_READ_WRITE, size, nullptr, &err);
    if (err != CL_SUCCESS || !m) {
        return PooledBuffer{}; // invalid guard
    }
    (void)_dev;
    return PooledBuffer(m, size, idx, this);
}

// ── allocate (bypass pool) ────────────────────────────────────────────────────

cl_mem OCLBufferPool::allocate(std::size_t bytes, cl_int* errOut)
{
    cl_int err = CL_SUCCESS;
    if (!_ctx) {
        if (errOut) { *errOut = CL_INVALID_CONTEXT; }
        return nullptr;
    }
    cl_mem buf = clCreateBuffer(_ctx, CL_MEM_READ_WRITE, bytes, nullptr, &err);
    if (errOut) { *errOut = err; }
    return buf;
}

// ── returnToPool (called by PooledBuffer destructor) ─────────────────────────

void OCLBufferPool::returnToPool(cl_mem mem, std::size_t bucketIdx)
{
    if (!mem) { return; }
    std::lock_guard<std::mutex> lock(_mtx);
    auto& freeList = _free[bucketIdx];
    if (freeList.size() < _maxCached) {
        freeList.push_back(mem);
    } else {
        clReleaseMemObject(mem);
    }
}

// ── diagnostics ──────────────────────────────────────────────────────────────

std::size_t OCLBufferPool::cachedCount() const
{
    std::lock_guard<std::mutex> lock(_mtx);
    std::size_t n = 0;
    for (const auto& freeList : _free) {
        n += freeList.size();
    }
    return n;
}

void OCLBufferPool::trimAll()
{
    std::lock_guard<std::mutex> lock(_mtx);
    for (auto& freeList : _free) {
        for (cl_mem m : freeList) {
            clReleaseMemObject(m);
        }
        freeList.clear();
    }
}

} // namespace opencl
} // namespace retdec
