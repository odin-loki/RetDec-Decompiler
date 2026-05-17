/**
 * @file src/ptx_decompile/cuda_host_recover.cpp
 * @brief OpenCL Host-Side Runtime Recovery implementation.
 *
 * Detects OpenCL API calls in SSA IR and reconstructs:
 *   - NDRange kernel enqueues  (clEnqueueNDRangeKernel / clEnqueueTask)
 *   - Buffer / image memory ops (clCreateBuffer, clEnqueueWriteBuffer, …)
 *   - Platform / device setup   (clGetDeviceIDs, clCreateContext, …)
 *   - Command queue management  (clCreateCommandQueue, clFinish, …)
 *   - Event synchronisation     (clCreateUserEvent, clWaitForEvents, …)
 *   - Program / kernel lifecycle (clBuildProgram, clCreateKernel, …)
 *   - SVM (Shared Virtual Memory, OpenCL 2.0+)
 *   - SPIR-V programs           (clCreateProgramWithIL)
 */

#include <memory>
#include "retdec/ptx_decompile/cuda_host_recover.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── Internal bridge ────────────────────────────────────────────────────────────

namespace {

struct OclCallEntry {
    std::string target;
    uint64_t    address = 0;
};

/// Extract direct-call entries from a real SSA basic block.
std::vector<OclCallEntry> extractOclCalls(const retdec::ssa::BasicBlock& blk) {
    std::vector<OclCallEntry> result;
    for (const retdec::ssa::IrInstr* instr : blk.instrs) {
        if (!instr) continue;
        if (instr->op == retdec::ssa::IrInstr::Op::Call &&
            !instr->calleeName.empty()) {
            result.push_back({instr->calleeName, instr->vma});
        }
    }
    return result;
}

/// Case-insensitive substring check.
bool ciContains(const std::string& h, const std::string& n) {
    if (n.empty()) return true;
    return std::search(h.begin(), h.end(), n.begin(), n.end(),
        [](char a, char b){
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        }) != h.end();
}

} // anonymous namespace

namespace retdec::ptx_decompile {

// ── Symbol tables ──────────────────────────────────────────────────────────────

// NDRange / task launch
static const std::unordered_set<std::string> kEnqueueKernel = {
    "clEnqueueNDRangeKernel",
    "clEnqueueTask",              // deprecated, equivalent to 1D NDRange
};

// Buffer operations
static const std::unordered_set<std::string> kCreateBuffer = {
    "clCreateBuffer", "clCreateSubBuffer",
};
static const std::unordered_set<std::string> kCreateImage = {
    "clCreateImage", "clCreateImage2D", "clCreateImage3D",
};
static const std::unordered_set<std::string> kReleaseMemObject = {
    "clReleaseMemObject",
};
static const std::unordered_set<std::string> kEnqueueWriteBuffer = {
    "clEnqueueWriteBuffer", "clEnqueueWriteBufferRect",
};
static const std::unordered_set<std::string> kEnqueueReadBuffer = {
    "clEnqueueReadBuffer", "clEnqueueReadBufferRect",
};
static const std::unordered_set<std::string> kEnqueueCopyBuffer = {
    "clEnqueueCopyBuffer", "clEnqueueCopyBufferRect",
};
static const std::unordered_set<std::string> kEnqueueFillBuffer = {
    "clEnqueueFillBuffer",
};
static const std::unordered_set<std::string> kEnqueueWriteImage = {
    "clEnqueueWriteImage",
};
static const std::unordered_set<std::string> kEnqueueReadImage = {
    "clEnqueueReadImage",
};
static const std::unordered_set<std::string> kEnqueueCopyImage = {
    "clEnqueueCopyImage", "clEnqueueCopyImageToBuffer", "clEnqueueCopyBufferToImage",
};
static const std::unordered_set<std::string> kEnqueueMap = {
    "clEnqueueMapBuffer", "clEnqueueMapImage",
};
static const std::unordered_set<std::string> kEnqueueUnmap = {
    "clEnqueueUnmapMemObject",
};

// SVM (OpenCL 2.0+)
static const std::unordered_set<std::string> kSvmAlloc = {
    "clSVMAlloc",
};
static const std::unordered_set<std::string> kSvmFree = {
    "clSVMFree",
};
static const std::unordered_set<std::string> kSvmOps = {
    "clEnqueueSVMMemcpy", "clEnqueueSVMMemFill",
    "clEnqueueSVMMap", "clEnqueueSVMUnmap",
    "clSetKernelArgSVMPointer",
};

// Platform / device / context
static const std::unordered_set<std::string> kPlatformDeviceOps = {
    "clGetPlatformIDs", "clGetPlatformInfo",
    "clGetDeviceIDs", "clGetDeviceInfo",
    "clCreateContext", "clCreateContextFromType", "clReleaseContext",
    "clRetainContext",
};

// Command queue
static const std::unordered_set<std::string> kQueueCreate = {
    "clCreateCommandQueue", "clCreateCommandQueueWithProperties",
};
static const std::unordered_set<std::string> kQueueRelease = {
    "clReleaseCommandQueue", "clRetainCommandQueue",
};
static const std::unordered_set<std::string> kQueueFinish = {
    "clFinish",
};
static const std::unordered_set<std::string> kQueueFlush = {
    "clFlush",
};

// Events
static const std::unordered_set<std::string> kEventCreate = {
    "clCreateUserEvent",
};
static const std::unordered_set<std::string> kEventSetStatus = {
    "clSetUserEventStatus",
};
static const std::unordered_set<std::string> kEventWait = {
    "clWaitForEvents",
};
static const std::unordered_set<std::string> kEventRelease = {
    "clReleaseEvent", "clRetainEvent",
};
static const std::unordered_set<std::string> kEventMarker = {
    "clEnqueueMarkerWithWaitList", "clEnqueueMarker",
};
static const std::unordered_set<std::string> kEventBarrier = {
    "clEnqueueBarrierWithWaitList", "clEnqueueBarrier",
};

// Program lifecycle
static const std::unordered_set<std::string> kProgramSource = {
    "clCreateProgramWithSource",
};
static const std::unordered_set<std::string> kProgramBinary = {
    "clCreateProgramWithBinary", "clCreateProgramWithBuiltInKernels",
};
static const std::unordered_set<std::string> kProgramIL = {
    "clCreateProgramWithIL",  // SPIR-V
};
static const std::unordered_set<std::string> kProgramBuild = {
    "clBuildProgram",
};
static const std::unordered_set<std::string> kProgramCompileLink = {
    "clCompileProgram", "clLinkProgram",
};
static const std::unordered_set<std::string> kProgramRelease = {
    "clReleaseProgram", "clRetainProgram",
};

// Kernel lifecycle
static const std::unordered_set<std::string> kKernelCreate = {
    "clCreateKernel", "clCreateKernelsInProgram",
};
static const std::unordered_set<std::string> kKernelSetArg = {
    "clSetKernelArg", "clSetKernelArgSVMPointer",
};
static const std::unordered_set<std::string> kKernelRelease = {
    "clReleaseKernel", "clRetainKernel",
};

// ── Helper ─────────────────────────────────────────────────────────────────────

static bool matchesAny(const std::string& sym,
                        const std::unordered_set<std::string>& set) {
    return set.count(sym) > 0 || [&](){
        for (const auto& s : set)
            if (sym.find(s) != std::string::npos) return true;
        return false;
    }();
}

// ── WorkSize ───────────────────────────────────────────────────────────────────

std::string WorkSize::str() const {
    if (!isKnown) return "{???}";
    if (dims <= 1)  return "{" + std::to_string(x) + "}";
    if (dims == 2)  return "{" + std::to_string(x) + "," + std::to_string(y) + "}";
    return "{" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + "}";
}

// ── OclHostModel ──────────────────────────────────────────────────────────────

void OclHostModel::merge(const OclHostModel& o) {
    enqueues.insert  (enqueues.end(),   o.enqueues.begin(),   o.enqueues.end());
    memOps.insert    (memOps.end(),     o.memOps.begin(),     o.memOps.end());
    deviceOps.insert (deviceOps.end(),  o.deviceOps.begin(),  o.deviceOps.end());
    queueOps.insert  (queueOps.end(),   o.queueOps.begin(),   o.queueOps.end());
    eventOps.insert  (eventOps.end(),   o.eventOps.begin(),   o.eventOps.end());
    kernelOps.insert (kernelOps.end(),  o.kernelOps.begin(),  o.kernelOps.end());
    if (!hasOpenCL) hasOpenCL = o.hasOpenCL;
    if (!usesSvm)   usesSvm   = o.usesSvm;
    if (!usesSpirV) usesSpirV = o.usesSpirV;
}

void OclHostModel::resolveKernelNames() {
    // Build map: kernelName → first enqueue that matches
    std::unordered_map<std::string, std::size_t> nameToEnqueue;
    for (std::size_t i = 0; i < enqueues.size(); ++i)
        if (!enqueues[i].kernelName.empty())
            nameToEnqueue.emplace(enqueues[i].kernelName, i);

    // For each clCreateKernel call, propagate the kernel name to matching enqueue
    for (const auto& ki : kernelOps) {
        if (ki.op != OclProgramOp::CreateKernel || ki.kernelName.empty()) continue;
        auto it = nameToEnqueue.find(ki.kernelName);
        if (it != nameToEnqueue.end())
            enqueues[it->second].kernelName = ki.kernelName;
    }
}

// ── KernelEnqueueDetector ──────────────────────────────────────────────────────

void KernelEnqueueDetector::analyseFunction(const ssa::SSAFunction& fn,
                                             OclHostModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractOclCalls(*blkPtr)) {
            if (!matchesAny(call.target, kEnqueueKernel)) continue;

            KernelEnqueue enq;
            enq.funcName  = fnName;
            enq.callAddr  = call.address;
            enq.isTask    = (call.target == "clEnqueueTask");
            enq.workDim   = enq.isTask ? 1 : 1;   // work_dim from arg2 if recoverable
            // Argument constant recovery requires dataflow analysis; leave
            // globalWorkSize / localWorkSize as unknown until that pass runs.
            out.enqueues.push_back(enq);
            out.hasOpenCL = true;
        }
    }
}

// ── OclMemoryDetector ─────────────────────────────────────────────────────────

void OclMemoryDetector::analyseFunction(const ssa::SSAFunction& fn,
                                         OclHostModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractOclCalls(*blkPtr)) {
            OclMemOpInfo info;
            info.funcName = fnName;
            info.callAddr = call.address;

            if (matchesAny(call.target, kCreateBuffer)) {
                info.op = OclMemOp::CreateBuffer;
            } else if (matchesAny(call.target, kCreateImage)) {
                info.op = OclMemOp::CreateImage;
            } else if (matchesAny(call.target, kReleaseMemObject)) {
                info.op = OclMemOp::ReleaseMemObject;
            } else if (matchesAny(call.target, kEnqueueWriteBuffer)) {
                info.op        = OclMemOp::EnqueueWriteBuffer;
                info.direction = OclCopyDir::HostToDevice;
                info.blocking  = true; // conservative default
            } else if (matchesAny(call.target, kEnqueueReadBuffer)) {
                info.op        = OclMemOp::EnqueueReadBuffer;
                info.direction = OclCopyDir::DeviceToHost;
                info.blocking  = true;
            } else if (matchesAny(call.target, kEnqueueCopyBuffer)) {
                info.op        = OclMemOp::EnqueueCopyBuffer;
                info.direction = OclCopyDir::DeviceToDevice;
                info.blocking  = false; // async by default in OpenCL
            } else if (matchesAny(call.target, kEnqueueFillBuffer)) {
                info.op = OclMemOp::EnqueueFillBuffer;
            } else if (matchesAny(call.target, kEnqueueWriteImage)) {
                info.op        = OclMemOp::EnqueueWriteImage;
                info.direction = OclCopyDir::HostToDevice;
            } else if (matchesAny(call.target, kEnqueueReadImage)) {
                info.op        = OclMemOp::EnqueueReadImage;
                info.direction = OclCopyDir::DeviceToHost;
            } else if (matchesAny(call.target, kEnqueueCopyImage)) {
                info.op        = OclMemOp::EnqueueCopyImage;
                info.direction = OclCopyDir::DeviceToDevice;
            } else if (matchesAny(call.target, kEnqueueMap)) {
                info.op = OclMemOp::EnqueueMapBuffer;
            } else if (matchesAny(call.target, kEnqueueUnmap)) {
                info.op = OclMemOp::EnqueueUnmapMemObject;
            } else if (matchesAny(call.target, kSvmAlloc)) {
                info.op = OclMemOp::SvmAlloc;
                out.usesSvm = true;
            } else if (matchesAny(call.target, kSvmFree)) {
                info.op = OclMemOp::SvmFree;
                out.usesSvm = true;
            } else if (matchesAny(call.target, kSvmOps)) {
                info.op = OclMemOp::EnqueueSvmMemcpy;
                out.usesSvm = true;
            } else {
                continue;
            }

            out.memOps.push_back(info);
            out.hasOpenCL = true;
        }
    }
}

// ── OclDeviceDetector ─────────────────────────────────────────────────────────

void OclDeviceDetector::analyseFunction(const ssa::SSAFunction& fn,
                                         OclHostModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractOclCalls(*blkPtr)) {
            if (!matchesAny(call.target, kPlatformDeviceOps)) continue;

            OclDeviceOpInfo info;
            info.apiName  = call.target;
            info.funcName = fnName;
            info.callAddr = call.address;
            // CL_DEVICE_TYPE_* constant recovery requires dataflow analysis.
            out.deviceOps.push_back(info);
            out.hasOpenCL = true;
        }
    }
}

// ── OclQueueEventDetector ─────────────────────────────────────────────────────

void OclQueueEventDetector::analyseFunction(const ssa::SSAFunction& fn,
                                             OclHostModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractOclCalls(*blkPtr)) {
            const auto& sym = call.target;

            if (matchesAny(sym, kQueueCreate)) {
                OclQueueOpInfo qi;
                qi.op       = OclQueueOp::Create;
                qi.funcName = fnName;
                qi.callAddr = call.address;
                out.queueOps.push_back(qi);
                out.hasOpenCL = true;
            } else if (matchesAny(sym, kQueueRelease)) {
                OclQueueOpInfo qi;
                qi.op       = OclQueueOp::Release;
                qi.funcName = fnName;
                qi.callAddr = call.address;
                out.queueOps.push_back(qi);
            } else if (matchesAny(sym, kQueueFinish)) {
                OclQueueOpInfo qi;
                qi.op       = OclQueueOp::Finish;
                qi.funcName = fnName;
                qi.callAddr = call.address;
                out.queueOps.push_back(qi);
                out.hasOpenCL = true;
            } else if (matchesAny(sym, kQueueFlush)) {
                OclQueueOpInfo qi;
                qi.op       = OclQueueOp::Flush;
                qi.funcName = fnName;
                qi.callAddr = call.address;
                out.queueOps.push_back(qi);
            } else if (matchesAny(sym, kEventCreate)) {
                OclEventOpInfo ei;
                ei.op       = OclEventOp::CreateUser;
                ei.funcName = fnName;
                ei.callAddr = call.address;
                out.eventOps.push_back(ei);
                out.hasOpenCL = true;
            } else if (matchesAny(sym, kEventSetStatus)) {
                OclEventOpInfo ei;
                ei.op       = OclEventOp::SetStatus;
                ei.funcName = fnName;
                ei.callAddr = call.address;
                out.eventOps.push_back(ei);
            } else if (matchesAny(sym, kEventWait)) {
                OclEventOpInfo ei;
                ei.op       = OclEventOp::Wait;
                ei.funcName = fnName;
                ei.callAddr = call.address;
                out.eventOps.push_back(ei);
                out.hasOpenCL = true;
            } else if (matchesAny(sym, kEventRelease)) {
                OclEventOpInfo ei;
                ei.op       = OclEventOp::Release;
                ei.funcName = fnName;
                ei.callAddr = call.address;
                out.eventOps.push_back(ei);
            } else if (matchesAny(sym, kEventMarker)) {
                OclEventOpInfo ei;
                ei.op       = OclEventOp::Marker;
                ei.funcName = fnName;
                ei.callAddr = call.address;
                out.eventOps.push_back(ei);
            } else if (matchesAny(sym, kEventBarrier)) {
                OclEventOpInfo ei;
                ei.op       = OclEventOp::Barrier;
                ei.funcName = fnName;
                ei.callAddr = call.address;
                out.eventOps.push_back(ei);
            }
        }
    }
}

// ── OclProgramDetector ────────────────────────────────────────────────────────

void OclProgramDetector::analyseFunction(const ssa::SSAFunction& fn,
                                          OclHostModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractOclCalls(*blkPtr)) {
            const auto& sym = call.target;

            OclKernelInfo ki;
            ki.funcName = fnName;
            ki.callAddr = call.address;

            if (matchesAny(sym, kProgramSource)) {
                ki.op = OclProgramOp::CreateProgramSource;
            } else if (matchesAny(sym, kProgramBinary)) {
                ki.op = OclProgramOp::CreateProgramBinary;
            } else if (matchesAny(sym, kProgramIL)) {
                ki.op = OclProgramOp::CreateProgramIL;
                out.usesSpirV = true;
            } else if (matchesAny(sym, kProgramBuild)) {
                ki.op = OclProgramOp::BuildProgram;
            } else if (matchesAny(sym, kProgramCompileLink)) {
                ki.op = ciContains(sym, "Link") ?
                        OclProgramOp::LinkProgram : OclProgramOp::CompileProgram;
            } else if (matchesAny(sym, kProgramRelease)) {
                ki.op = OclProgramOp::ReleaseProgram;
            } else if (matchesAny(sym, kKernelCreate)) {
                ki.op = OclProgramOp::CreateKernel;
                // clCreateKernel(program, "kernelName", &err) — arg1 is the
                // name string. Constant-propagation recovery is deferred.
            } else if (matchesAny(sym, kKernelSetArg)) {
                ki.op = OclProgramOp::SetKernelArg;
            } else if (matchesAny(sym, kKernelRelease)) {
                ki.op = OclProgramOp::ReleaseKernel;
            } else {
                continue;
            }

            out.kernelOps.push_back(ki);
            out.hasOpenCL = true;
        }
    }
}

// ── OclHostEmitter ────────────────────────────────────────────────────────────

static const char* oclMemOpStr(OclMemOp op) {
    switch (op) {
    case OclMemOp::CreateBuffer:          return "clCreateBuffer";
    case OclMemOp::CreateSubBuffer:       return "clCreateSubBuffer";
    case OclMemOp::CreateImage:           return "clCreateImage";
    case OclMemOp::ReleaseMemObject:      return "clReleaseMemObject";
    case OclMemOp::EnqueueWriteBuffer:    return "clEnqueueWriteBuffer";
    case OclMemOp::EnqueueReadBuffer:     return "clEnqueueReadBuffer";
    case OclMemOp::EnqueueCopyBuffer:     return "clEnqueueCopyBuffer";
    case OclMemOp::EnqueueFillBuffer:     return "clEnqueueFillBuffer";
    case OclMemOp::EnqueueWriteImage:     return "clEnqueueWriteImage";
    case OclMemOp::EnqueueReadImage:      return "clEnqueueReadImage";
    case OclMemOp::EnqueueCopyImage:      return "clEnqueueCopyImage";
    case OclMemOp::EnqueueMapBuffer:      return "clEnqueueMapBuffer";
    case OclMemOp::EnqueueUnmapMemObject: return "clEnqueueUnmapMemObject";
    case OclMemOp::SvmAlloc:              return "clSVMAlloc";
    case OclMemOp::SvmFree:               return "clSVMFree";
    case OclMemOp::EnqueueSvmMemcpy:      return "clEnqueueSVMMemcpy";
    case OclMemOp::EnqueueSvmMemFill:     return "clEnqueueSVMMemFill";
    default:                              return "clMemOp";
    }
}

static const char* oclProgramOpStr(OclProgramOp op) {
    switch (op) {
    case OclProgramOp::CreateProgramSource:  return "clCreateProgramWithSource";
    case OclProgramOp::CreateProgramBinary:  return "clCreateProgramWithBinary";
    case OclProgramOp::CreateProgramIL:      return "clCreateProgramWithIL";
    case OclProgramOp::BuildProgram:         return "clBuildProgram";
    case OclProgramOp::CompileProgram:       return "clCompileProgram";
    case OclProgramOp::LinkProgram:          return "clLinkProgram";
    case OclProgramOp::CreateKernel:         return "clCreateKernel";
    case OclProgramOp::SetKernelArg:         return "clSetKernelArg";
    case OclProgramOp::ReleaseKernel:        return "clReleaseKernel";
    case OclProgramOp::ReleaseProgram:       return "clReleaseProgram";
    default:                                 return "clProgramOp";
    }
}

std::string OclHostEmitter::emit(const OclHostModel& model) const {
    std::ostringstream os;
    os << "// OpenCL host code — recovered by RetDec\n";
    os << "// OpenCL present: " << (model.hasOpenCL ? "yes" : "no") << "\n";
    if (model.usesSvm)   os << "// SVM (Shared Virtual Memory) detected\n";
    if (model.usesSpirV) os << "// SPIR-V (clCreateProgramWithIL) detected\n";
    os << "\n";

    if (!model.kernelOps.empty()) {
        os << "// Program / kernel lifecycle (" << model.kernelOps.size() << "):\n";
        for (const auto& ki : model.kernelOps) {
            os << "//   " << oclProgramOpStr(ki.op);
            if (!ki.kernelName.empty()) os << "(\"" << ki.kernelName << "\")";
            if (!ki.buildOpts.empty())  os << "  opts=\"" << ki.buildOpts << "\"";
            os << "  // in " << ki.funcName
               << " @ 0x" << std::hex << ki.callAddr << std::dec << "\n";
        }
        os << "\n";
    }

    if (!model.enqueues.empty()) {
        os << "// NDRange enqueues (" << model.enqueues.size() << "):\n";
        for (const auto& e : model.enqueues) {
            os << "//   clEnqueue" << (e.isTask ? "Task" : "NDRangeKernel") << "(queue, ";
            if (!e.kernelName.empty()) os << e.kernelName;
            else                       os << "<kernel>";
            if (!e.isTask) {
                os << ", " << e.workDim
                   << ", NULL, "  << e.globalWorkSize.str()
                   << ", "        << (e.localWorkSize.isKnown ? e.localWorkSize.str() : "NULL");
            }
            os << ", ...)";
            os << "  // in " << e.funcName
               << " @ 0x" << std::hex << e.callAddr << std::dec << "\n";
        }
        os << "\n";
    }

    if (!model.memOps.empty()) {
        os << "// Memory operations (" << model.memOps.size() << "):\n";
        for (const auto& m : model.memOps) {
            os << "//   " << oclMemOpStr(m.op);
            if (m.sizeBytes)
                os << "(/* " << m.sizeBytes << " bytes */";
            else
                os << "(";
            if (m.direction == OclCopyDir::HostToDevice)   os << ", host→device";
            if (m.direction == OclCopyDir::DeviceToHost)   os << ", device→host";
            if (m.direction == OclCopyDir::DeviceToDevice) os << ", device→device";
            os << ")  // in " << m.funcName
               << " @ 0x" << std::hex << m.callAddr << std::dec << "\n";
        }
        os << "\n";
    }

    if (!model.deviceOps.empty()) {
        os << "// Platform/device ops (" << model.deviceOps.size() << "):\n";
        for (const auto& d : model.deviceOps) {
            os << "//   " << d.apiName
               << "  // in " << d.funcName
               << " @ 0x" << std::hex << d.callAddr << std::dec << "\n";
        }
        os << "\n";
    }

    if (!model.queueOps.empty()) {
        os << "// Command queue ops (" << model.queueOps.size() << "):\n";
        for (const auto& q : model.queueOps) {
            const char* opStr = (q.op == OclQueueOp::Create)   ? "clCreateCommandQueue" :
                                (q.op == OclQueueOp::Release)   ? "clReleaseCommandQueue":
                                (q.op == OclQueueOp::Finish)    ? "clFinish"             :
                                                                   "clFlush";
            os << "//   " << opStr
               << "  // in " << q.funcName
               << " @ 0x" << std::hex << q.callAddr << std::dec << "\n";
        }
        os << "\n";
    }

    if (!model.eventOps.empty()) {
        os << "// Event ops (" << model.eventOps.size() << "):\n";
        for (const auto& e : model.eventOps) {
            const char* opStr = (e.op == OclEventOp::CreateUser) ? "clCreateUserEvent"      :
                                (e.op == OclEventOp::SetStatus)  ? "clSetUserEventStatus"   :
                                (e.op == OclEventOp::Wait)       ? "clWaitForEvents"        :
                                (e.op == OclEventOp::Release)    ? "clReleaseEvent"         :
                                (e.op == OclEventOp::Marker)     ? "clEnqueueMarker"        :
                                                                    "clEnqueueBarrier";
            os << "//   " << opStr
               << "  // in " << e.funcName
               << " @ 0x" << std::hex << e.callAddr << std::dec << "\n";
        }
        os << "\n";
    }

    return os.str();
}

// ── OclHostRecovery (orchestrator) ────────────────────────────────────────────

OclHostRecovery::OclHostRecovery() {
    detectors_.push_back(std::make_unique<KernelEnqueueDetector>());
    detectors_.push_back(std::make_unique<OclMemoryDetector>());
    detectors_.push_back(std::make_unique<OclDeviceDetector>());
    detectors_.push_back(std::make_unique<OclQueueEventDetector>());
    detectors_.push_back(std::make_unique<OclProgramDetector>());
}

void OclHostRecovery::analyseFunction(const ssa::SSAFunction& fn,
                                       OclHostModel& out) {
    for (auto& d : detectors_)
        d->analyseFunction(fn, out);
}

OclHostModel OclHostRecovery::analyseModule(const ssa::SSAModule& mod) {
    OclHostModel result;
    for (const auto& fnPtr : mod.functions)
        if (fnPtr) analyseFunction(*fnPtr, result);
    result.resolveKernelNames();
    return result;
}


// --- CUDA Host Recovery Implementation ---------------------------------------

namespace {

struct CudaCallEntry {
    std::string              target;
    uint64_t                 address = 0;
    std::vector<std::string> strArgs;  ///< string constant args (empty if not constant)
    std::vector<uint64_t>    u64Args;  ///< integer constant args (0 if not constant)
    std::vector<bool>        isConst;  ///< which args are constants
};

std::vector<CudaCallEntry> extractCudaCalls(const retdec::ssa::BasicBlock& blk) {
    std::vector<CudaCallEntry> result;
    for (const retdec::ssa::IrInstr* instr : blk.instrs) {
        if (!instr) continue;
        if (instr->op != retdec::ssa::IrInstr::Op::Call) continue;
        if (instr->calleeName.empty()) continue;
        CudaCallEntry e;
        e.target  = instr->calleeName;
        e.address = instr->vma;
        result.push_back(std::move(e));
    }
    return result;
}

} // anonymous namespace

// Dim3::str
std::string Dim3::str() const {
    if (!isKnown) return "???";
    if (z > 1) return "dim3(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + ")";
    if (y > 1) return "dim3(" + std::to_string(x) + "," + std::to_string(y) + ")";
    return "dim3(" + std::to_string(x) + ")";
}

void CudaHostModel::merge(const CudaHostModel& o) {
    launches.insert(launches.end(), o.launches.begin(), o.launches.end());
    memOps.insert(memOps.end(), o.memOps.begin(), o.memOps.end());
    deviceOps.insert(deviceOps.end(), o.deviceOps.begin(), o.deviceOps.end());
    streamOps.insert(streamOps.end(), o.streamOps.begin(), o.streamOps.end());
    eventOps.insert(eventOps.end(), o.eventOps.begin(), o.eventOps.end());
    kernelRegs.insert(kernelRegs.end(), o.kernelRegs.begin(), o.kernelRegs.end());
    if (o.hasCuda) { hasCuda = true; primaryApi = o.primaryApi; }
    if (o.maxDeviceId > maxDeviceId) maxDeviceId = o.maxDeviceId;
}

// KernelLaunchDetector
void KernelLaunchDetector::analyseFunction(const ssa::SSAFunction& fn,
                                           CudaHostModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& ci : extractCudaCalls(*blkPtr)) {
            KernelLaunch l; l.funcName = fnName; l.callAddr = ci.address;
            if (ci.target == "cudaLaunchKernel") {
                l.api = CudaApi::RuntimeAPI;
                out.launches.push_back(l); out.hasCuda = true; out.primaryApi = CudaApi::RuntimeAPI;
            } else if (ci.target == "cuLaunchKernel") {
                l.api = CudaApi::DriverAPI;
                out.launches.push_back(l); out.hasCuda = true;
            } else if (ci.target == "cudaConfigureCall") {
                l.isLegacy = true; l.api = CudaApi::RuntimeAPI;
                out.launches.push_back(l); out.hasCuda = true;
            }
        }
    }
}

// CudaMemoryDetector
void CudaMemoryDetector::analyseFunction(const ssa::SSAFunction& fn,
                                         CudaHostModel& out) {
    static const std::unordered_set<std::string> kMemApis = {
        "cudaMalloc","cudaMallocManaged","cudaFree",
        "cudaMemcpy","cudaMemcpyAsync","cudaMemcpyToSymbol","cudaMemset"
    };
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& ci : extractCudaCalls(*blkPtr)) {
            if (!kMemApis.count(ci.target)) continue;
            CudaMemOpInfo m; m.funcName = fnName; m.callAddr = ci.address;
            if (ci.target == "cudaMalloc")          m.op = CudaMemOp::Malloc;
            else if (ci.target == "cudaMallocManaged") m.op = CudaMemOp::MallocManaged;
            else if (ci.target == "cudaFree")       m.op = CudaMemOp::Free;
            else if (ci.target == "cudaMemcpyToSymbol") m.op = CudaMemOp::MemcpyToSymbol;
            else if (ci.target == "cudaMemset")     m.op = CudaMemOp::Memset;
            else                                    m.op = CudaMemOp::Memcpy;
            out.memOps.push_back(m); out.hasCuda = true;
        }
    }
}

// CudaDeviceDetector
void CudaDeviceDetector::analyseFunction(const ssa::SSAFunction& fn,
                                         CudaHostModel& out) {
    static const std::unordered_set<std::string> kDevApis = {
        "cudaSetDevice","cudaGetDevice","cudaDeviceSynchronize",
        "cudaDeviceReset","cudaGetDeviceProperties","cudaDeviceGetAttribute"
    };
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& ci : extractCudaCalls(*blkPtr)) {
            if (!kDevApis.count(ci.target)) continue;
            CudaDeviceOpInfo d; d.apiName = ci.target; d.callAddr = ci.address;
            if (d.deviceId > out.maxDeviceId) out.maxDeviceId = d.deviceId;
            out.deviceOps.push_back(d); out.hasCuda = true;
            (void)fnName;
        }
    }
}

// CudaStreamEventDetector
void CudaStreamEventDetector::analyseFunction(const ssa::SSAFunction& fn,
                                              CudaHostModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& ci : extractCudaCalls(*blkPtr)) {
            if (ci.target == "cudaStreamCreate") {
                out.streamOps.push_back({CudaStreamOp::Create, fnName, ci.address}); out.hasCuda = true;
            } else if (ci.target == "cudaStreamSynchronize") {
                out.streamOps.push_back({CudaStreamOp::Synchronize, fnName, ci.address});
            } else if (ci.target == "cudaStreamDestroy") {
                out.streamOps.push_back({CudaStreamOp::Destroy, fnName, ci.address});
            } else if (ci.target == "cudaEventCreate") {
                out.eventOps.push_back({CudaEventOp::Create, fnName, ci.address}); out.hasCuda = true;
            } else if (ci.target == "cudaEventRecord") {
                out.eventOps.push_back({CudaEventOp::Record, fnName, ci.address});
            } else if (ci.target == "cudaEventSynchronize") {
                out.eventOps.push_back({CudaEventOp::Synchronize, fnName, ci.address});
            } else if (ci.target == "cudaEventDestroy") {
                out.eventOps.push_back({CudaEventOp::Destroy, fnName, ci.address});
            }
        }
    }
}

// NvccStubDetector
void NvccStubDetector::analyseFunction(const ssa::SSAFunction& fn,
                                       CudaHostModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& ci : extractCudaCalls(*blkPtr)) {
            if (ci.target == "__cudaRegisterFatBinary") {
                out.hasCuda = true;
            } else if (ci.target == "__cudaRegisterFunction") {
                KernelRegistration r;
                r.hostStubName = fnName;
                out.kernelRegs.push_back(r);
            }
        }
    }
}

// CudaHostEmitter
std::string CudaHostEmitter::emit(const CudaHostModel& m) const {
    std::ostringstream os;
    os << "// CUDA host code -- recovered by RetDec\n";
    os << "// CUDA present: " << (m.hasCuda ? "yes" : "no") << "\n";
    if (!m.hasCuda) return os.str();
    os << "// API: " << (m.primaryApi == CudaApi::DriverAPI ? "Driver (libcuda)" : "Runtime (libcudart)") << "\n";
    for (const auto& l : m.launches)
        os << "// Launch: " << l.kernelSym << " grid=" << l.gridDim.str() << " block=" << l.blockDim.str() << "\n";
    for (const auto& mo : m.memOps) {
        std::string n;
        switch (mo.op) {
        case CudaMemOp::Malloc:         n = "cudaMalloc";         break;
        case CudaMemOp::MallocManaged:  n = "cudaMallocManaged";  break;
        case CudaMemOp::Free:           n = "cudaFree";           break;
        case CudaMemOp::MemcpyToSymbol: n = "cudaMemcpyToSymbol"; break;
        default:                        n = "cudaMemcpy";         break;
        }
        os << "// MemOp: " << n;
        if (mo.sizeBytes) os << " size=" << mo.sizeBytes;
        if (mo.direction == MemcpyKind::HostToDevice)   os << " cudaMemcpyHostToDevice";
        if (mo.direction == MemcpyKind::DeviceToHost)   os << " cudaMemcpyDeviceToHost";
        os << "\n";
    }
    return os.str();
}

// CudaHostRecovery
CudaHostRecovery::CudaHostRecovery() {
    detectors_.push_back(std::make_unique<KernelLaunchDetector>());
    detectors_.push_back(std::make_unique<CudaMemoryDetector>());
    detectors_.push_back(std::make_unique<CudaDeviceDetector>());
    detectors_.push_back(std::make_unique<CudaStreamEventDetector>());
    detectors_.push_back(std::make_unique<NvccStubDetector>());
}

void CudaHostRecovery::analyseFunction(const ssa::SSAFunction& fn, CudaHostModel& out) {
    for (auto& d : detectors_) d->analyseFunction(fn, out);
}

CudaHostModel CudaHostRecovery::analyseModule(const ssa::SSAModule& mod) {
    CudaHostModel result;
    for (const auto& fnPtr : mod.functions)
        if (fnPtr) analyseFunction(*fnPtr, result);
    return result;
}

} // namespace retdec::ptx_decompile
