/**
 * @file include/retdec/opencl/ocl_context.h
 * @brief Host-side OpenCL context: device selection, kernel JIT + disk cache, buffer pool.
 *
 * Design:
 *  - Device scoring: GPU > CPU; within the same type prefer largest globalMem.
 *  - Kernel cache key = SHA-256(source + buildOptions + deviceVendor + driverVersion).
 *    Compiled binaries are stored in ~/.retdec/ocl-cache/<hash>.bin.
 *  - runKernel() attaches a cl_event and forwards timing to OCLProfiler::instance().
 */

#ifndef RETDEC_OPENCL_OCL_CONTEXT_H
#define RETDEC_OPENCL_OCL_CONTEXT_H

#include <CL/cl.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace retdec {
namespace opencl {

// ─── Device information snapshot ─────────────────────────────────────────────
struct OCLDeviceInfo {
    cl_device_id   id              = nullptr;
    cl_platform_id platformId      = nullptr;
    std::string    name;
    std::string    vendor;
    std::string    driverVersion;
    std::string    platformName;
    cl_device_type type            = CL_DEVICE_TYPE_CPU;
    std::uint64_t  globalMemBytes  = 0;
    std::uint64_t  localMemBytes   = 0;
    std::uint32_t  maxComputeUnits = 0;
    std::size_t    maxWorkGroupSize = 1;
    bool           available       = false;

    /// Higher score = better device.  GPU >  CPU; tie-break by globalMem.
    int score() const noexcept
    {
        int base = (type == CL_DEVICE_TYPE_GPU) ? 1000 : 0;
        // Add global mem in GiB (capped at 999 to stay below GPU/CPU gap).
        base += static_cast<int>(std::min<std::uint64_t>(globalMemBytes >> 30, 999u));
        return base;
    }
};

// ─── Kernel run descriptor ────────────────────────────────────────────────────
struct OCLKernelLaunch {
    std::string   programKey;     ///< key passed to ensureProgram()
    std::string   kernelName;     ///< __kernel function name
    std::size_t   globalSize  = 0;
    std::size_t   localSize   = 0; ///< 0 = let the runtime pick
    /// Called by caller to set kernel arguments before the launch.
    std::function<void(cl_kernel)> setArgs;
};

// ─── OCLContext ───────────────────────────────────────────────────────────────
class OCLContext {
public:
    OCLContext();
    ~OCLContext();

    OCLContext(OCLContext &&) noexcept;
    OCLContext &operator=(OCLContext &&) noexcept;

    OCLContext(const OCLContext &)            = delete;
    OCLContext &operator=(const OCLContext &) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /// Enumerate all OpenCL devices, score them, and create a context on the
    /// best one (highest score).  Returns true on success.
    bool initialize();

    /// Release the context, queues, and all cached programs.
    void reset();

    bool isReady() const noexcept;

    // ── Device query ─────────────────────────────────────────────────────────

    /// Info about the selected device (valid after initialize()).
    const OCLDeviceInfo& primaryDevice() const noexcept;

    /// Info about all enumerated devices (sorted best→worst).
    const std::vector<OCLDeviceInfo>& allDevices() const noexcept;

    // ── Program management ────────────────────────────────────────────────────

    /// Build (or restore from disk cache) an OpenCL program for @p cacheKey.
    /// The cache key is user-defined; the actual disk-cache key also hashes
    /// the source, buildOptions, and device driver version.
    ///
    /// @param buildLogOut  If non-null and build fails, receives the build log.
    /// @returns true if the program is ready to use.
    bool ensureProgram(const std::string& cacheKey,
                       const std::string& source,
                       const std::string& buildOptions = std::string{},
                       std::string*       buildLogOut  = nullptr);

    // ── Kernel execution with profiling ───────────────────────────────────────

    /// Enqueue an ND-range kernel launch and optionally wait for completion.
    /// Timing is recorded in OCLProfiler::instance() under kernelName.
    ///
    /// @returns CL_SUCCESS or a CL error code.
    cl_int runKernel(const OCLKernelLaunch& launch, bool blockUntilDone = true);

    // ── Buffer helpers ────────────────────────────────────────────────────────

    /// Allocate a device R/W buffer (CL_MEM_READ_WRITE).
    /// Use the OCLBufferPool for high-frequency allocations instead.
    cl_mem createBuffer(std::size_t bytes, cl_int* errOut = nullptr);

    /// Write host data → device buffer (blocking).
    cl_int writeBuffer(cl_mem buf, const void* src, std::size_t bytes);

    /// Read device buffer → host data (blocking).
    cl_int readBuffer(cl_mem buf, void* dst, std::size_t bytes);

    // ── Accessors for lower-level code ────────────────────────────────────────
    cl_context       clContext() const noexcept;
    cl_command_queue clQueue()   const noexcept;
    cl_device_id     clDevice()  const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace opencl
} // namespace retdec

#endif // RETDEC_OPENCL_OCL_CONTEXT_H
