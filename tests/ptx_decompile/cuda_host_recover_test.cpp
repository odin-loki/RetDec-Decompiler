/**
 * @file tests/ptx_decompile/cuda_host_recover_test.cpp
 * @brief Unit tests for the CUDA Host-Side Runtime Recovery.
 */

#include "retdec/ptx_decompile/cuda_host_recover.h"

#include <gtest/gtest.h>
#include <string>

using namespace retdec::ptx_decompile;
using namespace retdec;

// ── Stub types (must match those in the .cpp) ─────────────────────────────────

namespace retdec::ssa {
struct ConstArg {
    bool        isConst = false;
    uint64_t    value   = 0;
    std::string strVal;
};
struct CallInstr {
    std::string           target;
    uint64_t              address = 0;
    std::vector<ConstArg> args;
};
struct BasicBlock {
    std::vector<CallInstr> calls;
};
struct SSAFunction {
    std::string             name;
    uint64_t                address = 0;
    std::vector<BasicBlock> blocks;
};
struct SSAModule {
    std::vector<SSAFunction> functions;
};
} // namespace retdec::ssa

// ── Helpers ───────────────────────────────────────────────────────────────────

static ssa::ConstArg constU(uint64_t v) { ssa::ConstArg a; a.isConst=true; a.value=v; return a; }
static ssa::ConstArg constS(const std::string& s) { ssa::ConstArg a; a.isConst=true; a.strVal=s; return a; }
static ssa::ConstArg dynArg() { return {}; }

// ── KernelLaunchDetector tests ────────────────────────────────────────────────

TEST(KernelLaunchDetector, DetectsCudaLaunchKernel) {
    ssa::SSAFunction fn; fn.name = "host_main";
    ssa::BasicBlock blk;
    ssa::CallInstr ci; ci.target = "cudaLaunchKernel"; ci.address = 0x1000;
    ci.args = {constS("vectorAdd"), constU(0x0000000100000100ULL), constU(0x0000000100000100ULL),
               constU(0), dynArg()};
    blk.calls.push_back(ci);
    fn.blocks.push_back(blk);

    CudaHostModel model;
    KernelLaunchDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.launches.size(), 1u);
    EXPECT_EQ(model.launches[0].kernelSym, "vectorAdd");
    EXPECT_EQ(model.launches[0].api,       CudaApi::RuntimeAPI);
    EXPECT_TRUE(model.hasCuda);
    EXPECT_EQ(model.primaryApi, CudaApi::RuntimeAPI);
}

TEST(KernelLaunchDetector, DetectsCuLaunchKernelDriverAPI) {
    ssa::SSAFunction fn; fn.name = "driver_launch";
    ssa::BasicBlock blk;
    ssa::CallInstr ci; ci.target = "cuLaunchKernel"; ci.address = 0x2000;
    ci.args = {constS("myKernel"),
               constU(256), constU(1), constU(1),  // grid
               constU(256), constU(1), constU(1),  // block
               constU(0),                          // sharedMem
               dynArg(), dynArg(), dynArg()};       // stream, params, extra
    blk.calls.push_back(ci);
    fn.blocks.push_back(blk);

    CudaHostModel model;
    KernelLaunchDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.launches.size(), 1u);
    const auto& l = model.launches[0];
    EXPECT_EQ(l.api, CudaApi::DriverAPI);
    EXPECT_EQ(l.gridDim.x, 256u);
    EXPECT_EQ(l.gridDim.y, 1u);
    EXPECT_EQ(l.blockDim.x, 256u);
}

TEST(KernelLaunchDetector, DetectsLegacyCudaConfigureCall) {
    ssa::SSAFunction fn; fn.name = "old_launch";
    ssa::BasicBlock blk;
    blk.calls.push_back({"cudaConfigureCall", 0x3000, {}});
    fn.blocks.push_back(blk);

    CudaHostModel model;
    KernelLaunchDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.launches.size(), 1u);
    EXPECT_TRUE(model.launches[0].isLegacy);
}

TEST(KernelLaunchDetector, NoLaunchInNonCudaFunction) {
    ssa::SSAFunction fn; fn.name = "normal_fn";
    ssa::BasicBlock blk;
    blk.calls.push_back({"malloc", 0x4000, {}});
    fn.blocks.push_back(blk);

    CudaHostModel model;
    KernelLaunchDetector det;
    det.analyseFunction(fn, model);

    EXPECT_TRUE(model.launches.empty());
    EXPECT_FALSE(model.hasCuda);
}

// ── CudaMemoryDetector tests ──────────────────────────────────────────────────

TEST(CudaMemoryDetector, DetectsCudaMalloc) {
    ssa::SSAFunction fn; fn.name = "alloc_fn";
    ssa::BasicBlock blk;
    ssa::CallInstr ci; ci.target = "cudaMalloc"; ci.address = 0x5000;
    ci.args = {dynArg(), constU(1024)};
    blk.calls.push_back(ci);
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaMemoryDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.memOps.size(), 1u);
    EXPECT_EQ(model.memOps[0].op,        CudaMemOp::Malloc);
    EXPECT_EQ(model.memOps[0].sizeBytes, 1024u);
    EXPECT_TRUE(model.hasCuda);
}

TEST(CudaMemoryDetector, DetectsCudaMemcpyH2D) {
    ssa::SSAFunction fn; fn.name = "copy_fn";
    ssa::BasicBlock blk;
    ssa::CallInstr ci; ci.target = "cudaMemcpy"; ci.address = 0x6000;
    ci.args = {dynArg(), dynArg(), constU(4096), constU(1) /* H2D */};
    blk.calls.push_back(ci);
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaMemoryDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.memOps.size(), 1u);
    EXPECT_EQ(model.memOps[0].op,        CudaMemOp::Memcpy);
    EXPECT_EQ(model.memOps[0].direction, MemcpyKind::HostToDevice);
    EXPECT_EQ(model.memOps[0].sizeBytes, 4096u);
}

TEST(CudaMemoryDetector, DetectsCudaMemcpyD2H) {
    ssa::SSAFunction fn; fn.name = "read_back";
    ssa::BasicBlock blk;
    ssa::CallInstr ci; ci.target = "cudaMemcpy"; ci.address = 0x7000;
    ci.args = {dynArg(), dynArg(), constU(512), constU(2) /* D2H */};
    blk.calls.push_back(ci);
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaMemoryDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.memOps.size(), 1u);
    EXPECT_EQ(model.memOps[0].direction, MemcpyKind::DeviceToHost);
}

TEST(CudaMemoryDetector, DetectsCudaFree) {
    ssa::SSAFunction fn; fn.name = "free_fn";
    ssa::BasicBlock blk;
    blk.calls.push_back({"cudaFree", 0x8000, {dynArg()}});
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaMemoryDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.memOps.size(), 1u);
    EXPECT_EQ(model.memOps[0].op, CudaMemOp::Free);
}

TEST(CudaMemoryDetector, DetectsMemcpyToSymbol) {
    ssa::SSAFunction fn; fn.name = "sym_copy";
    ssa::BasicBlock blk;
    ssa::CallInstr ci; ci.target = "cudaMemcpyToSymbol"; ci.address = 0x9000;
    ci.args = {constS("d_coeffs"), dynArg(), constU(256), constU(0), constU(1)};
    blk.calls.push_back(ci);
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaMemoryDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.memOps.size(), 1u);
    EXPECT_EQ(model.memOps[0].op,         CudaMemOp::MemcpyToSymbol);
    EXPECT_EQ(model.memOps[0].symbolName, "d_coeffs");
}

TEST(CudaMemoryDetector, DetectsMallocManaged) {
    ssa::SSAFunction fn; fn.name = "unified_mem";
    ssa::BasicBlock blk;
    ssa::CallInstr ci; ci.target = "cudaMallocManaged"; ci.address = 0xa000;
    ci.args = {dynArg(), constU(2048)};
    blk.calls.push_back(ci);
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaMemoryDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.memOps.size(), 1u);
    EXPECT_EQ(model.memOps[0].op, CudaMemOp::MallocManaged);
}

// ── CudaDeviceDetector tests ──────────────────────────────────────────────────

TEST(CudaDeviceDetector, DetectsCudaSetDevice) {
    ssa::SSAFunction fn; fn.name = "select_gpu";
    ssa::BasicBlock blk;
    ssa::CallInstr ci; ci.target = "cudaSetDevice"; ci.address = 0xb000;
    ci.args = {constU(2)};
    blk.calls.push_back(ci);
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaDeviceDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.deviceOps.size(), 1u);
    EXPECT_EQ(model.deviceOps[0].deviceId, 2);
    EXPECT_EQ(model.maxDeviceId, 2);
    EXPECT_TRUE(model.hasCuda);
}

TEST(CudaDeviceDetector, DetectsCudaDeviceSynchronize) {
    ssa::SSAFunction fn; fn.name = "sync_fn";
    ssa::BasicBlock blk;
    blk.calls.push_back({"cudaDeviceSynchronize", 0xc000, {}});
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaDeviceDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.deviceOps.size(), 1u);
    EXPECT_EQ(model.deviceOps[0].apiName, "cudaDeviceSynchronize");
}

TEST(CudaDeviceDetector, MaxDeviceIdTracked) {
    ssa::SSAModule mod;
    for (int d : {0, 3, 1, 7, 2}) {
        ssa::SSAFunction fn; fn.name = "fn_" + std::to_string(d);
        ssa::BasicBlock blk;
        ssa::CallInstr ci; ci.target = "cudaSetDevice";
        ci.args = {constU(static_cast<uint64_t>(d))};
        blk.calls.push_back(ci);
        fn.blocks.push_back(blk);
        mod.functions.push_back(fn);
    }
    CudaHostRecovery rec;
    auto model = rec.analyseModule(mod);
    EXPECT_EQ(model.maxDeviceId, 7);
}

// ── CudaStreamEventDetector tests ─────────────────────────────────────────────

TEST(CudaStreamEventDetector, DetectsStreamCreate) {
    ssa::SSAFunction fn; fn.name = "stream_test";
    ssa::BasicBlock blk;
    blk.calls.push_back({"cudaStreamCreate", 0xd000, {}});
    blk.calls.push_back({"cudaStreamSynchronize", 0xd010, {}});
    blk.calls.push_back({"cudaStreamDestroy", 0xd020, {}});
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaStreamEventDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.streamOps.size(), 3u);
    EXPECT_EQ(model.streamOps[0].op, CudaStreamOp::Create);
    EXPECT_EQ(model.streamOps[1].op, CudaStreamOp::Synchronize);
    EXPECT_EQ(model.streamOps[2].op, CudaStreamOp::Destroy);
}

TEST(CudaStreamEventDetector, DetectsEventLifecycle) {
    ssa::SSAFunction fn; fn.name = "event_test";
    ssa::BasicBlock blk;
    blk.calls.push_back({"cudaEventCreate",      0xe000, {}});
    blk.calls.push_back({"cudaEventRecord",      0xe010, {}});
    blk.calls.push_back({"cudaEventSynchronize", 0xe020, {}});
    blk.calls.push_back({"cudaEventDestroy",     0xe030, {}});
    fn.blocks.push_back(blk);

    CudaHostModel model;
    CudaStreamEventDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.eventOps.size(), 4u);
    EXPECT_EQ(model.eventOps[0].op, CudaEventOp::Create);
    EXPECT_EQ(model.eventOps[1].op, CudaEventOp::Record);
    EXPECT_EQ(model.eventOps[2].op, CudaEventOp::Synchronize);
    EXPECT_EQ(model.eventOps[3].op, CudaEventOp::Destroy);
}

// ── NvccStubDetector tests ────────────────────────────────────────────────────

TEST(NvccStubDetector, DetectsFatBinaryAndRegisterFunction) {
    ssa::SSAFunction fn; fn.name = "__cuda_module_init";
    ssa::BasicBlock blk;
    {
        ssa::CallInstr ci; ci.target = "__cudaRegisterFatBinary"; ci.address = 0xf000;
        ci.args = {constU(0x500000)};
        blk.calls.push_back(ci);
    }
    {
        ssa::CallInstr ci; ci.target = "__cudaRegisterFunction"; ci.address = 0xf010;
        ci.args = {dynArg(), constS("vectorAdd"), constS("_Z9vectorAddPKfS0_Pfi"),
                   dynArg(), dynArg(), dynArg(), dynArg(), dynArg(), dynArg()};
        blk.calls.push_back(ci);
    }
    fn.blocks.push_back(blk);

    CudaHostModel model;
    NvccStubDetector det;
    det.analyseFunction(fn, model);

    EXPECT_TRUE(model.hasCuda);
    ASSERT_EQ(model.kernelRegs.size(), 1u);
    EXPECT_EQ(model.kernelRegs[0].hostStubName, "vectorAdd");
    EXPECT_EQ(model.kernelRegs[0].ptxFuncName,  "_Z9vectorAddPKfS0_Pfi");
    EXPECT_EQ(model.kernelRegs[0].fatbinaryAddr, 0x500000u);
}

// ── CudaHostRecovery (orchestrator) ───────────────────────────────────────────

TEST(CudaHostRecovery, AnalysesComplexModule) {
    ssa::SSAModule mod;

    // Module init function
    {
        ssa::SSAFunction fn; fn.name = "__cuda_init";
        ssa::BasicBlock blk;
        {
            ssa::CallInstr ci; ci.target = "__cudaRegisterFatBinary";
            ci.args = {constU(0x600000)};
            blk.calls.push_back(ci);
        }
        {
            ssa::CallInstr ci; ci.target = "__cudaRegisterFunction";
            ci.args = {dynArg(), constS("matMul"), constS("_Z6matMulPfS_S_i"),
                       dynArg(), dynArg(), dynArg(), dynArg(), dynArg(), dynArg()};
            blk.calls.push_back(ci);
        }
        fn.blocks.push_back(blk);
        mod.functions.push_back(fn);
    }

    // Main function
    {
        ssa::SSAFunction fn; fn.name = "main";
        ssa::BasicBlock blk;
        blk.calls.push_back({"cudaSetDevice", 0x1000, {constU(0)}});
        {
            ssa::CallInstr ci; ci.target = "cudaMalloc";
            ci.args = {dynArg(), constU(4096)};
            blk.calls.push_back(ci);
        }
        {
            ssa::CallInstr ci; ci.target = "cudaMemcpy";
            ci.args = {dynArg(), dynArg(), constU(4096), constU(1)};
            blk.calls.push_back(ci);
        }
        {
            ssa::CallInstr ci; ci.target = "cudaLaunchKernel";
            ci.args = {constS("matMul"), constU(0x0000000100000040ULL),
                       constU(0x0000000100000010ULL), constU(1024), dynArg()};
            blk.calls.push_back(ci);
        }
        blk.calls.push_back({"cudaDeviceSynchronize", 0x2000, {}});
        blk.calls.push_back({"cudaFree", 0x2010, {dynArg()}});
        fn.blocks.push_back(blk);
        mod.functions.push_back(fn);
    }

    CudaHostRecovery rec;
    auto model = rec.analyseModule(mod);

    EXPECT_TRUE(model.hasCuda);
    EXPECT_EQ(model.primaryApi, CudaApi::RuntimeAPI);
    EXPECT_GE(model.launches.size(), 1u);
    EXPECT_GE(model.memOps.size(), 3u);   // malloc + memcpy + free
    EXPECT_GE(model.deviceOps.size(), 2u); // setDevice + sync
    EXPECT_GE(model.kernelRegs.size(), 1u);
}

TEST(CudaHostRecovery, EmptyModuleNotCuda) {
    ssa::SSAModule mod;
    CudaHostRecovery rec;
    auto model = rec.analyseModule(mod);
    EXPECT_FALSE(model.hasCuda);
}

// ── CudaHostModel::merge tests ────────────────────────────────────────────────

TEST(CudaHostModel, MergeEmpty) {
    CudaHostModel a, b;
    a.merge(b);
    EXPECT_FALSE(a.hasCuda);
}

TEST(CudaHostModel, MergePreservesLaunches) {
    CudaHostModel a, b;
    KernelLaunch kl; kl.kernelSym = "myKernel"; kl.funcName = "f";
    b.launches.push_back(kl);
    b.hasCuda    = true;
    b.primaryApi = CudaApi::RuntimeAPI;
    a.merge(b);
    ASSERT_EQ(a.launches.size(), 1u);
    EXPECT_EQ(a.launches[0].kernelSym, "myKernel");
    EXPECT_TRUE(a.hasCuda);
}

// ── Dim3::str tests ───────────────────────────────────────────────────────────

TEST(Dim3, StrUnknown) {
    Dim3 d;
    EXPECT_EQ(d.str(), "???");
}

TEST(Dim3, Str1D) {
    Dim3 d; d.x=128; d.isKnown=true;
    EXPECT_EQ(d.str(), "dim3(128)");
}

TEST(Dim3, Str2D) {
    Dim3 d; d.x=32; d.y=16; d.isKnown=true;
    EXPECT_EQ(d.str(), "dim3(32,16)");
}

TEST(Dim3, Str3D) {
    Dim3 d; d.x=8; d.y=8; d.z=4; d.isKnown=true;
    EXPECT_EQ(d.str(), "dim3(8,8,4)");
}

// ── CudaHostEmitter tests ─────────────────────────────────────────────────────

TEST(CudaHostEmitter, EmitEmptyModel) {
    CudaHostModel model;
    CudaHostEmitter emitter;
    auto s = emitter.emit(model);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("CUDA present: no"), std::string::npos);
}

TEST(CudaHostEmitter, EmitWithLaunch) {
    CudaHostModel model;
    model.hasCuda    = true;
    model.primaryApi = CudaApi::RuntimeAPI;
    KernelLaunch l;
    l.kernelSym = "vectorAdd";
    l.gridDim   = {256, 1, 1, true};
    l.blockDim  = {256, 1, 1, true};
    l.funcName  = "main";
    l.callAddr  = 0x1234;
    model.launches.push_back(l);

    CudaHostEmitter emitter;
    auto s = emitter.emit(model);
    EXPECT_NE(s.find("vectorAdd"),              std::string::npos);
    EXPECT_NE(s.find("Runtime (libcudart)"),    std::string::npos);
    EXPECT_NE(s.find("dim3(256)"),              std::string::npos);
}

TEST(CudaHostEmitter, EmitWithMemOp) {
    CudaHostModel model;
    model.hasCuda = true;
    CudaMemOpInfo m;
    m.op        = CudaMemOp::Memcpy;
    m.direction = MemcpyKind::HostToDevice;
    m.sizeBytes = 512;
    m.funcName  = "init";
    model.memOps.push_back(m);

    CudaHostEmitter emitter;
    auto s = emitter.emit(model);
    EXPECT_NE(s.find("cudaMemcpy"),             std::string::npos);
    EXPECT_NE(s.find("cudaMemcpyHostToDevice"), std::string::npos);
    EXPECT_NE(s.find("512"),                    std::string::npos);
}
