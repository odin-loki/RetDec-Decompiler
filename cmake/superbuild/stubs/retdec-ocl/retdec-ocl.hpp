#pragma once

// RetDec OpenCL host layer — public API.
//
// When RETDEC_OCL_AVAILABLE=1 (OpenCL runtime found at build time) the real
// OCLContext implementation in src/opencl/ provides full device management,
// kernel JIT compilation with disk caching, and a buffer pool.
//
// When RETDEC_OCL_AVAILABLE=0 the stub always returns sensible defaults so
// higher-level code compiles and runs without an OpenCL runtime installed.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::ocl {

enum class DeviceType {
    GPU,
    CPU,
    Accelerator,
    Unknown,
};

struct DeviceInfo {
    std::string name;
    DeviceType  type          = DeviceType::Unknown;
    std::size_t globalMemBytes = 0;
    std::size_t maxWorkGroupSize = 1;
    bool        available     = false;
};

// ─── OCLContext ───────────────────────────────────────────────────────────────
// Central OpenCL context manager. One instance per process.
//
// Typical usage:
//   auto& ctx = OCLContext::instance();
//   if (!ctx.initialize())  { /* fall back to CPU path */ }
//   auto prog = ctx.buildProgram(kernelSource, "-cl-fast-relaxed-math");
class OCLContext {
public:
    static OCLContext& instance();

    // Enumerate platforms/devices and create a context on the best device.
    // Returns true on success, false if no OpenCL runtime is present.
    bool initialize();

    // Returns false if initialize() was not called or failed.
    bool isAvailable() const noexcept { return _available; }

    // Best available device (populated after initialize()).
    const DeviceInfo& primaryDevice() const noexcept { return _primary; }

    // All enumerated devices.
    const std::vector<DeviceInfo>& allDevices() const noexcept { return _devices; }

    // Build a program from source. Returns empty string on error (check buildLog()).
    // Result is cached: same (source, options) pair → reuse compiled binary.
    std::string buildProgram(std::string_view source, std::string_view buildOptions = {});

    // Human-readable log from the last buildProgram() call.
    const std::string& buildLog() const noexcept { return _buildLog; }

private:
    OCLContext() = default;

    bool                    _available = false;
    DeviceInfo              _primary;
    std::vector<DeviceInfo> _devices;
    std::string             _buildLog;
};

} // namespace retdec::ocl

// Backwards-compatibility alias used by existing code.
namespace retdec {
    using OCLContext = ocl::OCLContext;
}
