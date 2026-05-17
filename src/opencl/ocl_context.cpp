/**
 * @file src/opencl/ocl_context.cpp
 * @brief OCLContext implementation: device scoring, JIT compilation, disk cache, profiling.
 */

#include <memory>
#include "retdec/opencl/ocl_context.h"
#include "retdec/opencl/ocl_error.h"
#include "retdec/opencl/ocl_profiler.h"
#include "ocl_disk_cache_internal.h"

#include <CL/cl.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace opencl {
namespace {

// ─── Device info query helpers ────────────────────────────────────────────────

template<typename T>
static T deviceInfoT(cl_device_id dev, cl_device_info param, T fallback = T{})
{
    T val{};
    if (clGetDeviceInfo(dev, param, sizeof(T), &val, nullptr) != CL_SUCCESS) {
        return fallback;
    }
    return val;
}

static std::string deviceInfoStr(cl_device_id dev, cl_device_info param)
{
    size_t n = 0;
    if (clGetDeviceInfo(dev, param, 0, nullptr, &n) != CL_SUCCESS || n == 0) {
        return {};
    }
    std::string s(n, '\0');
    clGetDeviceInfo(dev, param, n, s.data(), nullptr);
    // Remove trailing null
    while (!s.empty() && s.back() == '\0') { s.pop_back(); }
    return s;
}

static std::string platformInfoStr(cl_platform_id plat, cl_platform_info param)
{
    size_t n = 0;
    if (clGetPlatformInfo(plat, param, 0, nullptr, &n) != CL_SUCCESS || n == 0) {
        return {};
    }
    std::string s(n, '\0');
    clGetPlatformInfo(plat, param, n, s.data(), nullptr);
    while (!s.empty() && s.back() == '\0') { s.pop_back(); }
    return s;
}

// ─── Enumerate all devices ────────────────────────────────────────────────────

static std::vector<OCLDeviceInfo> enumerateAllDevices()
{
    cl_uint nPlat = 0;
    if (clGetPlatformIDs(0, nullptr, &nPlat) != CL_SUCCESS || nPlat == 0) {
        return {};
    }

    std::vector<cl_platform_id> platforms(nPlat);
    if (clGetPlatformIDs(nPlat, platforms.data(), nullptr) != CL_SUCCESS) {
        return {};
    }

    std::vector<OCLDeviceInfo> devices;

    for (auto plat : platforms) {
        const std::string platName = platformInfoStr(plat, CL_PLATFORM_NAME);

        cl_uint nDev = 0;
        if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, 0, nullptr, &nDev) != CL_SUCCESS
            || nDev == 0) {
            continue;
        }
        std::vector<cl_device_id> devIds(nDev);
        if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, nDev, devIds.data(), nullptr) != CL_SUCCESS) {
            continue;
        }

        for (auto devId : devIds) {
            cl_bool avail = CL_FALSE;
            clGetDeviceInfo(devId, CL_DEVICE_AVAILABLE, sizeof(avail), &avail, nullptr);

            OCLDeviceInfo info;
            info.id            = devId;
            info.platformId    = plat;
            info.name          = deviceInfoStr(devId, CL_DEVICE_NAME);
            info.vendor        = deviceInfoStr(devId, CL_DEVICE_VENDOR);
            info.driverVersion = deviceInfoStr(devId, CL_DRIVER_VERSION);
            info.platformName  = platName;
            info.type          = deviceInfoT<cl_device_type>(devId, CL_DEVICE_TYPE);
            info.globalMemBytes = deviceInfoT<cl_ulong>(devId, CL_DEVICE_GLOBAL_MEM_SIZE);
            info.localMemBytes  = deviceInfoT<cl_ulong>(devId, CL_DEVICE_LOCAL_MEM_SIZE);
            info.maxComputeUnits = deviceInfoT<cl_uint>(devId, CL_DEVICE_MAX_COMPUTE_UNITS);
            info.maxWorkGroupSize = deviceInfoT<size_t>(devId, CL_DEVICE_MAX_WORK_GROUP_SIZE, 1u);
            info.available     = (avail == CL_TRUE);

            devices.push_back(std::move(info));
        }
    }

    // Sort best → worst
    std::sort(devices.begin(), devices.end(),
        [](const OCLDeviceInfo& a, const OCLDeviceInfo& b) {
            return a.score() > b.score();
        });

    return devices;
}

// ─── Cache key: hash source + buildOptions + driverVersion ───────────────────

static std::uint64_t fnv1a64(std::string_view s) noexcept
{
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime  = 1099511628211ULL;
    std::uint64_t h = offset;
    for (unsigned char c : s) {
        h ^= static_cast<std::uint64_t>(c);
        h *= prime;
    }
    return h;
}

static std::string diskCacheKey(const std::string& userKey,
                                const std::string& source,
                                const std::string& buildOptions,
                                const std::string& driverVersion)
{
    // Combine all four pieces so that changing any one invalidates the cache.
    std::uint64_t h = fnv1a64(userKey);
    h ^= fnv1a64(source)        * 0x9e3779b97f4a7c15ULL;
    h ^= fnv1a64(buildOptions)  * 0x6c62272e07bb0142ULL;
    h ^= fnv1a64(driverVersion) * 0x94d049bb133111ebULL;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buf);
}

// ─── Save / restore compiled binary ──────────────────────────────────────────

static bool saveBinary(cl_program prog, cl_device_id dev,
                       const std::string& compositeKey)
{
    size_t binSize = 0;
    if (clGetProgramInfo(prog, CL_PROGRAM_BINARY_SIZES,
                         sizeof(binSize), &binSize, nullptr) != CL_SUCCESS
        || binSize == 0) {
        return false;
    }
    std::vector<unsigned char> blob(binSize);
    unsigned char* ptr     = blob.data();
    unsigned char* ptrList = ptr;
    if (clGetProgramInfo(prog, CL_PROGRAM_BINARIES,
                         sizeof(unsigned char*), &ptrList, nullptr) != CL_SUCCESS) {
        return false;
    }
    (void)dev;
    return detail::saveProgramBinary(compositeKey, blob);
}

} // namespace

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct OCLContext::Impl {
    std::vector<OCLDeviceInfo>             allDevices;
    OCLDeviceInfo                          primary;

    cl_context       context  = nullptr;
    cl_command_queue queue    = nullptr;

    std::unordered_map<std::string, cl_program> programs;

    ~Impl() { reset(); }

    void reset()
    {
        for (auto& [k, p] : programs) {
            if (p) { clReleaseProgram(p); }
        }
        programs.clear();

        if (queue)   { clReleaseCommandQueue(queue);  queue   = nullptr; }
        if (context) { clReleaseContext(context);     context = nullptr; }
        primary = {};
        allDevices.clear();
    }
};

// ─── OCLContext ───────────────────────────────────────────────────────────────

OCLContext::OCLContext()  : _impl(std::make_unique<Impl>()) {}
OCLContext::~OCLContext() = default;

OCLContext::OCLContext(OCLContext&&) noexcept            = default;
OCLContext& OCLContext::operator=(OCLContext&&) noexcept = default;

bool OCLContext::initialize()
{
    if (!_impl) { return false; }
    _impl->reset();

    _impl->allDevices = enumerateAllDevices();
    if (_impl->allDevices.empty()) { return false; }

    // Pick the best available device
    const OCLDeviceInfo* chosen = nullptr;
    for (const auto& d : _impl->allDevices) {
        if (d.available) { chosen = &d; break; }
    }
    if (!chosen) { return false; }

    _impl->primary = *chosen;

    cl_context_properties props[] = {
        CL_CONTEXT_PLATFORM,
        reinterpret_cast<cl_context_properties>(chosen->platformId),
        0,
    };

    cl_int err = CL_SUCCESS;
    _impl->context = clCreateContext(props, 1, &chosen->id, nullptr, nullptr, &err);
    if (err != CL_SUCCESS || !_impl->context) {
        _impl->reset();
        return false;
    }

#if defined(CL_VERSION_2_0)
    cl_queue_properties qprops[] = {
        CL_QUEUE_PROPERTIES,
        static_cast<cl_queue_properties>(CL_QUEUE_PROFILING_ENABLE),
        0,
    };
    _impl->queue = clCreateCommandQueueWithProperties(
        _impl->context, chosen->id, qprops, &err);
#else
    _impl->queue = clCreateCommandQueue(
        _impl->context, chosen->id, CL_QUEUE_PROFILING_ENABLE, &err);
#endif
    if (err != CL_SUCCESS || !_impl->queue) {
        _impl->reset();
        return false;
    }

    return true;
}

void OCLContext::reset()
{
    if (_impl) { _impl->reset(); }
}

bool OCLContext::isReady() const noexcept
{
    return _impl && _impl->context && _impl->queue && _impl->primary.id;
}

const OCLDeviceInfo& OCLContext::primaryDevice() const noexcept
{
    static const OCLDeviceInfo kEmpty;
    return _impl ? _impl->primary : kEmpty;
}

const std::vector<OCLDeviceInfo>& OCLContext::allDevices() const noexcept
{
    static const std::vector<OCLDeviceInfo> kEmpty;
    return _impl ? _impl->allDevices : kEmpty;
}

cl_context       OCLContext::clContext() const noexcept { return _impl ? _impl->context : nullptr; }
cl_command_queue OCLContext::clQueue()   const noexcept { return _impl ? _impl->queue   : nullptr; }
cl_device_id     OCLContext::clDevice()  const noexcept { return _impl ? _impl->primary.id : nullptr; }

// ─── ensureProgram ────────────────────────────────────────────────────────────

bool OCLContext::ensureProgram(const std::string& cacheKey,
                               const std::string& source,
                               const std::string& buildOptions,
                               std::string*       buildLogOut)
{
    if (!isReady() || cacheKey.empty()) {
        if (buildLogOut) { *buildLogOut = "OCLContext not initialized"; }
        return false;
    }

    // Already compiled this session?
    auto it = _impl->programs.find(cacheKey);
    if (it != _impl->programs.end() && it->second) {
        return true;
    }

    const std::string compositeKey = diskCacheKey(
        cacheKey, source, buildOptions, _impl->primary.driverVersion);

    // ── Try disk cache first ──────────────────────────────────────────────────
    std::vector<unsigned char> cachedBin;
    if (detail::loadProgramBinary(compositeKey, cachedBin) && !cachedBin.empty()) {
        cl_int binStat = CL_SUCCESS;
        cl_int err     = CL_SUCCESS;
        const unsigned char* bp   = cachedBin.data();
        const size_t         blen = cachedBin.size();
        cl_program prog = clCreateProgramWithBinary(
            _impl->context, 1, &_impl->primary.id,
            &blen, &bp, &binStat, &err);

        if (prog && err == CL_SUCCESS && binStat == CL_SUCCESS) {
            const char* opts = buildOptions.empty() ? nullptr : buildOptions.c_str();
            err = clBuildProgram(prog, 1, &_impl->primary.id, opts, nullptr, nullptr);
            if (err == CL_SUCCESS) {
                _impl->programs[cacheKey] = prog;
                if (buildLogOut) { buildLogOut->clear(); }
                return true;
            }
            clReleaseProgram(prog);
        } else if (prog) {
            clReleaseProgram(prog);
        }
        // Cache miss / stale binary — fall through to JIT compile
    }

    // ── JIT compile from source ───────────────────────────────────────────────
    const char* src = source.c_str();
    const size_t len = source.size();
    cl_int err = CL_SUCCESS;
    cl_program prog = clCreateProgramWithSource(
        _impl->context, 1, &src, &len, &err);
    if (err != CL_SUCCESS || !prog) {
        if (buildLogOut) { *buildLogOut = clErrorString(err); }
        return false;
    }

    const char* opts = buildOptions.empty() ? nullptr : buildOptions.c_str();
    err = clBuildProgram(prog, 1, &_impl->primary.id, opts, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        std::string log;
        size_t logSize = 0;
        if (clGetProgramBuildInfo(prog, _impl->primary.id, CL_PROGRAM_BUILD_LOG,
                                  0, nullptr, &logSize) == CL_SUCCESS && logSize > 0) {
            std::vector<char> buf(logSize + 1);
            clGetProgramBuildInfo(prog, _impl->primary.id, CL_PROGRAM_BUILD_LOG,
                                  buf.size(), buf.data(), nullptr);
            log.assign(buf.data());
        }
        clReleaseProgram(prog);
        if (buildLogOut) {
            *buildLogOut = log.empty() ? clErrorString(err) : log;
        }
        return false;
    }

    _impl->programs[cacheKey] = prog;
    saveBinary(prog, _impl->primary.id, compositeKey);
    if (buildLogOut) { buildLogOut->clear(); }
    return true;
}

// ─── runKernel ────────────────────────────────────────────────────────────────

cl_int OCLContext::runKernel(const OCLKernelLaunch& launch, bool blockUntilDone)
{
    if (!isReady()) { return CL_INVALID_CONTEXT; }

    auto it = _impl->programs.find(launch.programKey);
    if (it == _impl->programs.end() || !it->second) {
        return CL_INVALID_PROGRAM;
    }

    cl_int err = CL_SUCCESS;
    cl_kernel kernel = clCreateKernel(it->second, launch.kernelName.c_str(), &err);
    if (err != CL_SUCCESS || !kernel) { return err; }

    if (launch.setArgs) {
        launch.setArgs(kernel);
    }

    const size_t gs = launch.globalSize;
    const size_t* ls = (launch.localSize > 0) ? &launch.localSize : nullptr;

    cl_event ev = nullptr;
    err = clEnqueueNDRangeKernel(
        _impl->queue, kernel, 1, nullptr, &gs, ls, 0, nullptr, &ev);

    if (err == CL_SUCCESS && ev) {
        if (blockUntilDone) {
            clWaitForEvents(1, &ev);
        }
        // Record timing in profiler
        OCLProfiler::instance().recordEvent(launch.kernelName.c_str(), ev);
        clReleaseEvent(ev);
    }

    clReleaseKernel(kernel);
    return err;
}

// ─── Buffer helpers ───────────────────────────────────────────────────────────

cl_mem OCLContext::createBuffer(std::size_t bytes, cl_int* errOut)
{
    if (!isReady()) {
        if (errOut) { *errOut = CL_INVALID_CONTEXT; }
        return nullptr;
    }
    cl_int err = CL_SUCCESS;
    cl_mem buf = clCreateBuffer(_impl->context, CL_MEM_READ_WRITE, bytes, nullptr, &err);
    if (errOut) { *errOut = err; }
    return buf;
}

cl_int OCLContext::writeBuffer(cl_mem buf, const void* src, std::size_t bytes)
{
    if (!isReady()) { return CL_INVALID_CONTEXT; }
    return clEnqueueWriteBuffer(
        _impl->queue, buf, CL_TRUE, 0, bytes, src, 0, nullptr, nullptr);
}

cl_int OCLContext::readBuffer(cl_mem buf, void* dst, std::size_t bytes)
{
    if (!isReady()) { return CL_INVALID_CONTEXT; }
    return clEnqueueReadBuffer(
        _impl->queue, buf, CL_TRUE, 0, bytes, dst, 0, nullptr, nullptr);
}

} // namespace opencl
} // namespace retdec
