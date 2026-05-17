/**
 * @file include/retdec/ptx_decompile/cuda_host_recover.h
 * @brief OpenCL Host-Side Runtime Recovery — Stage 42.
 *
 * Identifies and recovers OpenCL host-side runtime API calls from binary code,
 * reconstructing kernel enqueues, memory management operations, and device/
 * platform management patterns.
 *
 * ## Detection Scope
 *
 * ### Platform and device discovery
 *
 *   `clGetPlatformIDs`, `clGetPlatformInfo`,
 *   `clGetDeviceIDs`, `clGetDeviceInfo`,
 *   `clCreateContext`, `clCreateContextFromType`, `clReleaseContext`.
 *
 * ### Command queue (equivalent to CUDA streams)
 *
 *   `clCreateCommandQueue`, `clCreateCommandQueueWithProperties`,
 *   `clReleaseCommandQueue`, `clFinish`, `clFlush`.
 *
 * ### Memory management
 *
 *   Buffer allocation:
 *     `clCreateBuffer`, `clCreateSubBuffer`, `clReleaseMemObject`.
 *
 *   Image allocation:
 *     `clCreateImage`, `clCreateImage2D` (deprecated), `clCreateImage3D` (deprecated).
 *
 *   Buffer I/O:
 *     `clEnqueueWriteBuffer`, `clEnqueueReadBuffer`,
 *     `clEnqueueCopyBuffer`, `clEnqueueFillBuffer`,
 *     `clEnqueueWriteBufferRect`, `clEnqueueReadBufferRect`.
 *
 *   Mapping:
 *     `clEnqueueMapBuffer`, `clEnqueueMapImage`, `clEnqueueUnmapMemObject`.
 *
 *   Image I/O:
 *     `clEnqueueWriteImage`, `clEnqueueReadImage`, `clEnqueueCopyImage`.
 *
 *   SVM (Shared Virtual Memory, OpenCL 2.0+):
 *     `clSVMAlloc`, `clSVMFree`,
 *     `clEnqueueSVMMemcpy`, `clEnqueueSVMMemFill`,
 *     `clEnqueueSVMMap`, `clEnqueueSVMUnmap`.
 *
 * ### Program and kernel management
 *
 *   `clCreateProgramWithSource`, `clCreateProgramWithBinary`,
 *   `clCreateProgramWithBuiltInKernels`, `clCreateProgramWithIL` (SPIR-V),
 *   `clBuildProgram`, `clCompileProgram`, `clLinkProgram`,
 *   `clReleaseProgram`, `clGetProgramInfo`, `clGetProgramBuildInfo`,
 *   `clCreateKernel`, `clCreateKernelsInProgram`,
 *   `clSetKernelArg`, `clSetKernelArgSVMPointer`,
 *   `clReleaseKernel`, `clGetKernelInfo`, `clGetKernelWorkGroupInfo`.
 *
 * ### Kernel enqueue (NDRange launch)
 *
 *   `clEnqueueNDRangeKernel`:
 *     arg0 = cl_command_queue,
 *     arg1 = cl_kernel,
 *     arg2 = work_dim (uint),
 *     arg3 = global_work_offset (size_t*, nullable),
 *     arg4 = global_work_size  (size_t[work_dim]),
 *     arg5 = local_work_size   (size_t[work_dim], nullable),
 *     arg6 = num_events_in_wait_list,
 *     arg7 = event_wait_list,
 *     arg8 = event (cl_event*, nullable).
 *
 *   `clEnqueueTask` (deprecated, equivalent to 1D NDRange with global=local=1).
 *
 * ### Synchronisation and events
 *
 *   `clCreateUserEvent`, `clSetUserEventStatus`,
 *   `clWaitForEvents`, `clGetEventInfo`, `clReleaseEvent`,
 *   `clSetEventCallback`,
 *   `clEnqueueMarkerWithWaitList`, `clEnqueueBarrierWithWaitList`.
 *
 * ## Output
 *
 *   OclHostModel — top-level result containing:
 *     KernelEnqueue   — recovered kernel enqueue sites
 *     OclMemOpInfo    — clCreateBuffer / clReleaseMemObject / clEnqueueWriteBuffer calls
 *     OclDeviceOpInfo — platform and device management calls
 *     OclQueueOpInfo  — command queue create/release/sync
 *     OclEventOpInfo  — event create/wait/release
 *     OclKernelInfo   — clCreateProgramWithSource / clBuildProgram / clCreateKernel calls
 *
 *   OclHostEmitter — emits reconstructed OpenCL C host code skeleton.
 */

#ifndef RETDEC_PTX_DECOMPILE_CUDA_HOST_RECOVER_H
#define RETDEC_PTX_DECOMPILE_CUDA_HOST_RECOVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retdec::ssa {
struct SSAFunction;
struct SSAModule;
}

namespace retdec::ptx_decompile {

// ─── Enumerations ─────────────────────────────────────────────────────────────

/// Which OpenCL memory-management call family was detected.
enum class OclMemOp {
    CreateBuffer,         ///< clCreateBuffer
    CreateSubBuffer,      ///< clCreateSubBuffer
    CreateImage,          ///< clCreateImage / clCreateImage2D / clCreateImage3D
    ReleaseMemObject,     ///< clReleaseMemObject
    EnqueueWriteBuffer,   ///< clEnqueueWriteBuffer / clEnqueueWriteBufferRect
    EnqueueReadBuffer,    ///< clEnqueueReadBuffer  / clEnqueueReadBufferRect
    EnqueueCopyBuffer,    ///< clEnqueueCopyBuffer
    EnqueueFillBuffer,    ///< clEnqueueFillBuffer
    EnqueueWriteImage,    ///< clEnqueueWriteImage
    EnqueueReadImage,     ///< clEnqueueReadImage
    EnqueueCopyImage,     ///< clEnqueueCopyImage
    EnqueueMapBuffer,     ///< clEnqueueMapBuffer
    EnqueueMapImage,      ///< clEnqueueMapImage
    EnqueueUnmapMemObject,///< clEnqueueUnmapMemObject
    SvmAlloc,             ///< clSVMAlloc
    SvmFree,              ///< clSVMFree
    EnqueueSvmMemcpy,     ///< clEnqueueSVMMemcpy
    EnqueueSvmMemFill,    ///< clEnqueueSVMMemFill
    Unknown,
};

/// Transfer direction for buffer/image copies.
enum class OclCopyDir {
    Unknown,
    HostToDevice,   ///< clEnqueueWriteBuffer
    DeviceToHost,   ///< clEnqueueReadBuffer
    DeviceToDevice, ///< clEnqueueCopyBuffer
};

/// Command queue (stream) operation kinds.
enum class OclQueueOp {
    Create,       ///< clCreateCommandQueue / clCreateCommandQueueWithProperties
    Release,      ///< clReleaseCommandQueue
    Finish,       ///< clFinish
    Flush,        ///< clFlush
};

/// Event operation kinds.
enum class OclEventOp {
    CreateUser,   ///< clCreateUserEvent
    SetStatus,    ///< clSetUserEventStatus
    Wait,         ///< clWaitForEvents
    Release,      ///< clReleaseEvent
    Marker,       ///< clEnqueueMarkerWithWaitList
    Barrier,      ///< clEnqueueBarrierWithWaitList
};

/// Program/kernel lifecycle operation kinds.
enum class OclProgramOp {
    CreateProgramSource,  ///< clCreateProgramWithSource
    CreateProgramBinary,  ///< clCreateProgramWithBinary
    CreateProgramIL,      ///< clCreateProgramWithIL (SPIR-V)
    BuildProgram,         ///< clBuildProgram
    CompileProgram,       ///< clCompileProgram
    LinkProgram,          ///< clLinkProgram
    CreateKernel,         ///< clCreateKernel
    SetKernelArg,         ///< clSetKernelArg
    ReleaseKernel,        ///< clReleaseKernel
    ReleaseProgram,       ///< clReleaseProgram
};

// ─── Work-size structure ──────────────────────────────────────────────────────

/**
 * @brief Recovered NDRange work sizes (global or local).
 *
 * OpenCL uses flat `size_t[]` arrays; we store up to 3 dimensions.
 */
struct WorkSize {
    uint64_t x = 0, y = 0, z = 0;  ///< 0 = unknown
    uint32_t dims = 0;              ///< work_dim argument (1, 2, or 3)
    bool     isKnown = false;

    std::string str() const;
};

// ─── Recovered structures ─────────────────────────────────────────────────────

/**
 * @brief A recovered clEnqueueNDRangeKernel (or clEnqueueTask) call.
 */
struct KernelEnqueue {
    std::string funcName;              ///< containing host function
    uint64_t    callAddr     = 0;      ///< VMA of the enqueue call
    std::string kernelName;            ///< cl_kernel symbol (if recoverable)
    WorkSize    globalWorkSize;        ///< global_work_size[]
    WorkSize    localWorkSize;         ///< local_work_size[] (may be null → driver chooses)
    uint32_t    workDim      = 1;      ///< work_dim argument
    bool        hasWaitList  = false;  ///< num_events_in_wait_list > 0
    bool        hasEvent     = false;  ///< output cl_event* != NULL
    bool        isTask       = false;  ///< legacy clEnqueueTask
};

/**
 * @brief A recovered OpenCL memory operation (buffer / image / SVM).
 */
struct OclMemOpInfo {
    OclMemOp    op           = OclMemOp::Unknown;
    std::string funcName;
    uint64_t    callAddr     = 0;
    uint64_t    sizeBytes    = 0;    ///< size argument (if constant)
    OclCopyDir  direction    = OclCopyDir::Unknown;
    uint64_t    flags        = 0;    ///< cl_mem_flags argument (if constant)
    bool        blocking     = true; ///< CL_TRUE / CL_FALSE blocking flag
};

/**
 * @brief A recovered OpenCL platform / device management call.
 */
struct OclDeviceOpInfo {
    std::string apiName;             ///< e.g. "clGetDeviceIDs"
    std::string funcName;
    uint64_t    callAddr     = 0;
    uint64_t    deviceType   = 0;    ///< CL_DEVICE_TYPE_* (if constant)
};

/**
 * @brief A recovered OpenCL command queue operation.
 */
struct OclQueueOpInfo {
    OclQueueOp  op           = OclQueueOp::Create;
    std::string funcName;
    uint64_t    callAddr     = 0;
    uint64_t    properties   = 0;    ///< queue property flags (if constant)
};

/**
 * @brief A recovered OpenCL event operation.
 */
struct OclEventOpInfo {
    OclEventOp  op           = OclEventOp::CreateUser;
    std::string funcName;
    uint64_t    callAddr     = 0;
    uint32_t    numEvents    = 0;    ///< num_events_in_wait_list (if constant)
};

/**
 * @brief A recovered OpenCL program or kernel lifecycle call.
 */
struct OclKernelInfo {
    OclProgramOp op          = OclProgramOp::CreateProgramSource;
    std::string  funcName;
    uint64_t     callAddr    = 0;
    std::string  kernelName; ///< kernel name string for clCreateKernel
    std::string  buildOpts;  ///< options string for clBuildProgram (if recoverable)
};

// ─── OclHostModel ─────────────────────────────────────────────────────────────

/**
 * @brief Full recovered OpenCL host-side model for the analysed binary.
 */
struct OclHostModel {
    std::vector<KernelEnqueue>  enqueues;
    std::vector<OclMemOpInfo>   memOps;
    std::vector<OclDeviceOpInfo> deviceOps;
    std::vector<OclQueueOpInfo>  queueOps;
    std::vector<OclEventOpInfo>  eventOps;
    std::vector<OclKernelInfo>   kernelOps;

    bool     hasOpenCL   = false;
    bool     usesSvm     = false;   ///< true if any SVM API calls detected
    bool     usesSpirV   = false;   ///< true if clCreateProgramWithIL detected

    void merge(const OclHostModel& other);

    /**
     * @brief Attempt to match clCreateKernel names to enqueue sites.
     *        Fills `KernelEnqueue::kernelName` for matched enqueues.
     */
    void resolveKernelNames();
};

// ─── Detector interface ───────────────────────────────────────────────────────

class IOclHostDetector {
public:
    virtual ~IOclHostDetector() = default;
    virtual void analyseFunction(const retdec::ssa::SSAFunction& fn,
                                 OclHostModel& out) = 0;
};

// ─── Concrete detectors ───────────────────────────────────────────────────────

/**
 * @brief Detects clEnqueueNDRangeKernel and legacy clEnqueueTask calls.
 */
class KernelEnqueueDetector : public IOclHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         OclHostModel& out) override;
};

/**
 * @brief Detects clCreateBuffer, clReleaseMemObject, clEnqueueWriteBuffer, etc.
 */
class OclMemoryDetector : public IOclHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         OclHostModel& out) override;
};

/**
 * @brief Detects platform and device management calls (clGetDeviceIDs, clCreateContext, …).
 */
class OclDeviceDetector : public IOclHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         OclHostModel& out) override;
};

/**
 * @brief Detects command queue and event calls.
 */
class OclQueueEventDetector : public IOclHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         OclHostModel& out) override;
};

/**
 * @brief Detects program/kernel lifecycle calls (clBuildProgram, clCreateKernel, …).
 */
class OclProgramDetector : public IOclHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         OclHostModel& out) override;
};

// ─── Emitter ─────────────────────────────────────────────────────────────────

/**
 * @brief Emits a reconstructed OpenCL C host code skeleton.
 *
 * Example output:
 *
 *   // OpenCL host code — recovered by RetDec
 *   //
 *   // Program build:
 *   //   clBuildProgram(program, ...) → kernel "vectorAdd"
 *   //
 *   // NDRange launches:
 *   //   clEnqueueNDRangeKernel(queue, vectorAdd, 1, NULL,
 *   //       {256}, {64}, 0, NULL, NULL);
 *   //
 *   // Memory ops:
 *   //   cl_mem d_a = clCreateBuffer(ctx, CL_MEM_READ_WRITE, 1024, NULL, &err);
 */
class OclHostEmitter {
public:
    std::string emit(const OclHostModel& model) const;
};

// ─── OclHostRecovery (orchestrator) ──────────────────────────────────────────

/**
 * @brief Runs all OpenCL host detectors over every function in an SSA module.
 */
class OclHostRecovery {
public:
    OclHostRecovery();

    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         OclHostModel& out);

    OclHostModel analyseModule(const retdec::ssa::SSAModule& mod);

private:
    std::vector<std::unique_ptr<IOclHostDetector>> detectors_;
};

// ─── CUDA Host Recovery Types ────────────────────────────────────────────────

enum class CudaApi { RuntimeAPI, DriverAPI };

enum class CudaMemOp {
    Malloc, MallocManaged, Free, Memcpy, MemcpyAsync,
    MemcpyToSymbol, MemcpyFromSymbol, Memset
};

enum class MemcpyKind { HostToDevice, DeviceToHost, DeviceToDevice, Default };

enum class CudaStreamOp { Create, Destroy, Synchronize, Wait };
enum class CudaEventOp  { Create, Destroy, Record, Synchronize, ElapsedTime };

struct Dim3 {
    uint32_t x = 0, y = 0, z = 0;
    bool     isKnown = false;
    std::string str() const;
};

struct KernelLaunch {
    std::string kernelSym;
    std::string funcName;
    uint64_t    callAddr  = 0;
    CudaApi     api       = CudaApi::RuntimeAPI;
    bool        isLegacy  = false;
    Dim3        gridDim;
    Dim3        blockDim;
    uint64_t    sharedMem = 0;
};

struct CudaMemOpInfo {
    CudaMemOp   op          = CudaMemOp::Malloc;
    MemcpyKind  direction   = MemcpyKind::Default;
    uint64_t    sizeBytes   = 0;
    std::string symbolName;
    std::string funcName;
    uint64_t    callAddr    = 0;
};

struct CudaDeviceOpInfo {
    std::string apiName;
    int         deviceId  = 0;
    uint64_t    callAddr  = 0;
};

struct CudaStreamOpInfo {
    CudaStreamOp op       = CudaStreamOp::Create;
    std::string  funcName;
    uint64_t     callAddr = 0;
};

struct CudaEventOpInfo {
    CudaEventOp op        = CudaEventOp::Create;
    std::string funcName;
    uint64_t    callAddr  = 0;
};

struct KernelRegistration {
    std::string hostStubName;
    std::string ptxFuncName;
    uint64_t    fatbinaryAddr = 0;
};

struct CudaHostModel {
    std::vector<KernelLaunch>       launches;
    std::vector<CudaMemOpInfo>      memOps;
    std::vector<CudaDeviceOpInfo>   deviceOps;
    std::vector<CudaStreamOpInfo>   streamOps;
    std::vector<CudaEventOpInfo>    eventOps;
    std::vector<KernelRegistration> kernelRegs;

    bool    hasCuda     = false;
    int     maxDeviceId = -1;
    CudaApi primaryApi  = CudaApi::RuntimeAPI;

    void merge(const CudaHostModel& other);
};

// ─── CUDA detector interface ──────────────────────────────────────────────────

class ICudaHostDetector {
public:
    virtual ~ICudaHostDetector() = default;
    virtual void analyseFunction(const retdec::ssa::SSAFunction& fn,
                                 CudaHostModel& out) = 0;
};

class KernelLaunchDetector : public ICudaHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         CudaHostModel& out) override;
};

class CudaMemoryDetector : public ICudaHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         CudaHostModel& out) override;
};

class CudaDeviceDetector : public ICudaHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         CudaHostModel& out) override;
};

class CudaStreamEventDetector : public ICudaHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         CudaHostModel& out) override;
};

class NvccStubDetector : public ICudaHostDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         CudaHostModel& out) override;
};

class CudaHostEmitter {
public:
    std::string emit(const CudaHostModel& model) const;
};

class CudaHostRecovery {
public:
    CudaHostRecovery();
    void analyseFunction(const retdec::ssa::SSAFunction& fn, CudaHostModel& out);
    CudaHostModel analyseModule(const retdec::ssa::SSAModule& mod);
private:
    std::vector<std::unique_ptr<ICudaHostDetector>> detectors_;
};

} // namespace retdec::ptx_decompile

#endif // RETDEC_PTX_DECOMPILE_CUDA_HOST_RECOVER_H
