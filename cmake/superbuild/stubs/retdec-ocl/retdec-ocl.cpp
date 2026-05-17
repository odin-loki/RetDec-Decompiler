#include "retdec-ocl.hpp"

// Stub implementation of the RetDec OpenCL host layer.
// The real implementation lives in src/opencl/ and is built when
// -DRETDEC_ENABLE_OPENCL=ON is passed to the core RetDec build.

namespace retdec::ocl {

OCLContext& OCLContext::instance()
{
    static OCLContext inst;
    return inst;
}

bool OCLContext::initialize()
{
#if RETDEC_OCL_AVAILABLE
    // Real OpenCL runtime is linked: attempt platform/device enumeration.
    // This stub returns false — the full implementation in src/opencl/ocl_context.cpp
    // performs the actual cl_platform_id / cl_device_id enumeration and scoring.
    _available = false;
    _buildLog  = "Stub: real OCLContext not linked (build RetDec with -DRETDEC_ENABLE_OPENCL=ON).";
    return false;
#else
    _available = false;
    _buildLog  = "OpenCL runtime not available at build time.";
    return false;
#endif
}

std::string OCLContext::buildProgram(std::string_view /*source*/,
                                     std::string_view /*buildOptions*/)
{
    _buildLog = "Stub: buildProgram() is a no-op. Enable real OpenCL layer.";
    return {};
}

} // namespace retdec::ocl
