/**
 * @file tests/ptx_decompile/cuda_host_recover_test.cpp
 * @brief Unit tests for the CUDA Host-Side Runtime Recovery.
 *
 * Detectors match callee names on real SSA Call instructions. Argument
 * recovery (kernel symbol, dims, sizes) is not wired in extractCudaCalls.
 */

#include "retdec/ptx_decompile/cuda_host_recover.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

using namespace retdec::ptx_decompile;
using namespace retdec::ssa;

namespace {

std::unique_ptr<SSAFunction> makeFnWithCalls(
		const std::string& name,
		std::initializer_list<std::pair<const char*, uint64_t>> calls)
{
	auto fn = std::make_unique<SSAFunction>(name);
	fn->addBlock("entry");
	for (const auto& [callee, vma] : calls) {
		IrInstr* i = fn->addInstr(0, IrInstr::Op::Call, vma);
		i->calleeName = callee;
	}
	return fn;
}

} // namespace

// ── KernelLaunchDetector tests ────────────────────────────────────────────────

TEST(KernelLaunchDetector, DetectsCudaLaunchKernel) {
	auto fn = makeFnWithCalls("host_main", {{"cudaLaunchKernel", 0x1000}});

	CudaHostModel model;
	KernelLaunchDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.launches.size(), 1u);
	EXPECT_EQ(model.launches[0].api, CudaApi::RuntimeAPI);
	EXPECT_EQ(model.launches[0].callAddr, 0x1000u);
	EXPECT_TRUE(model.hasCuda);
	EXPECT_EQ(model.primaryApi, CudaApi::RuntimeAPI);
}

TEST(KernelLaunchDetector, DetectsCuLaunchKernelDriverAPI) {
	auto fn = makeFnWithCalls("driver_launch", {{"cuLaunchKernel", 0x2000}});

	CudaHostModel model;
	KernelLaunchDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.launches.size(), 1u);
	EXPECT_EQ(model.launches[0].api, CudaApi::DriverAPI);
	EXPECT_EQ(model.launches[0].callAddr, 0x2000u);
}

TEST(KernelLaunchDetector, DetectsLegacyCudaConfigureCall) {
	auto fn = makeFnWithCalls("old_launch", {{"cudaConfigureCall", 0x3000}});

	CudaHostModel model;
	KernelLaunchDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.launches.size(), 1u);
	EXPECT_TRUE(model.launches[0].isLegacy);
}

TEST(KernelLaunchDetector, NoLaunchInNonCudaFunction) {
	auto fn = makeFnWithCalls("normal_fn", {{"malloc", 0x4000}});

	CudaHostModel model;
	KernelLaunchDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_TRUE(model.launches.empty());
	EXPECT_FALSE(model.hasCuda);
}

// ── CudaMemoryDetector tests ──────────────────────────────────────────────────

TEST(CudaMemoryDetector, DetectsCudaMalloc) {
	auto fn = makeFnWithCalls("alloc_fn", {{"cudaMalloc", 0x5000}});

	CudaHostModel model;
	CudaMemoryDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.memOps.size(), 1u);
	EXPECT_EQ(model.memOps[0].op, CudaMemOp::Malloc);
	EXPECT_EQ(model.memOps[0].callAddr, 0x5000u);
	EXPECT_TRUE(model.hasCuda);
}

TEST(CudaMemoryDetector, DetectsCudaMemcpyH2D) {
	auto fn = makeFnWithCalls("copy_fn", {{"cudaMemcpy", 0x6000}});

	CudaHostModel model;
	CudaMemoryDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.memOps.size(), 1u);
	EXPECT_EQ(model.memOps[0].op, CudaMemOp::Memcpy);
}

TEST(CudaMemoryDetector, DetectsCudaMemcpyD2H) {
	auto fn = makeFnWithCalls("read_back", {{"cudaMemcpy", 0x7000}});

	CudaHostModel model;
	CudaMemoryDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.memOps.size(), 1u);
	EXPECT_EQ(model.memOps[0].op, CudaMemOp::Memcpy);
}

TEST(CudaMemoryDetector, DetectsCudaFree) {
	auto fn = makeFnWithCalls("free_fn", {{"cudaFree", 0x8000}});

	CudaHostModel model;
	CudaMemoryDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.memOps.size(), 1u);
	EXPECT_EQ(model.memOps[0].op, CudaMemOp::Free);
}

TEST(CudaMemoryDetector, DetectsMemcpyToSymbol) {
	auto fn = makeFnWithCalls("sym_copy", {{"cudaMemcpyToSymbol", 0x9000}});

	CudaHostModel model;
	CudaMemoryDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.memOps.size(), 1u);
	EXPECT_EQ(model.memOps[0].op, CudaMemOp::MemcpyToSymbol);
}

TEST(CudaMemoryDetector, DetectsMallocManaged) {
	auto fn = makeFnWithCalls("unified_mem", {{"cudaMallocManaged", 0xa000}});

	CudaHostModel model;
	CudaMemoryDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.memOps.size(), 1u);
	EXPECT_EQ(model.memOps[0].op, CudaMemOp::MallocManaged);
}

// ── CudaDeviceDetector tests ──────────────────────────────────────────────────

TEST(CudaDeviceDetector, DetectsCudaSetDevice) {
	auto fn = makeFnWithCalls("select_gpu", {{"cudaSetDevice", 0xb000}});

	CudaHostModel model;
	CudaDeviceDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.deviceOps.size(), 1u);
	EXPECT_EQ(model.deviceOps[0].apiName, "cudaSetDevice");
	EXPECT_EQ(model.deviceOps[0].callAddr, 0xb000u);
	EXPECT_TRUE(model.hasCuda);
}

TEST(CudaDeviceDetector, DetectsCudaDeviceSynchronize) {
	auto fn = makeFnWithCalls("sync_fn", {{"cudaDeviceSynchronize", 0xc000}});

	CudaHostModel model;
	CudaDeviceDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.deviceOps.size(), 1u);
	EXPECT_EQ(model.deviceOps[0].apiName, "cudaDeviceSynchronize");
}

TEST(CudaDeviceDetector, MaxDeviceIdTracked) {
	SSAModule mod;
	for (int d : {0, 3, 1, 7, 2}) {
		SSAFunction* fn = mod.addFunction("fn_" + std::to_string(d));
		fn->addBlock("entry");
		IrInstr* ci = fn->addInstr(0, IrInstr::Op::Call);
		ci->calleeName = "cudaSetDevice";
		(void)d;
	}
	CudaHostRecovery rec;
	auto model = rec.analyseModule(mod);
	EXPECT_EQ(model.deviceOps.size(), 5u);
	EXPECT_TRUE(model.hasCuda);
}

// ── CudaStreamEventDetector tests ─────────────────────────────────────────────

TEST(CudaStreamEventDetector, DetectsStreamCreate) {
	auto fn = makeFnWithCalls("stream_test", {
			{"cudaStreamCreate", 0xd000},
			{"cudaStreamSynchronize", 0xd010},
			{"cudaStreamDestroy", 0xd020},
	});

	CudaHostModel model;
	CudaStreamEventDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.streamOps.size(), 3u);
	EXPECT_EQ(model.streamOps[0].op, CudaStreamOp::Create);
	EXPECT_EQ(model.streamOps[1].op, CudaStreamOp::Synchronize);
	EXPECT_EQ(model.streamOps[2].op, CudaStreamOp::Destroy);
}

TEST(CudaStreamEventDetector, DetectsEventLifecycle) {
	auto fn = makeFnWithCalls("event_test", {
			{"cudaEventCreate", 0xe000},
			{"cudaEventRecord", 0xe010},
			{"cudaEventSynchronize", 0xe020},
			{"cudaEventDestroy", 0xe030},
	});

	CudaHostModel model;
	CudaStreamEventDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.eventOps.size(), 4u);
	EXPECT_EQ(model.eventOps[0].op, CudaEventOp::Create);
	EXPECT_EQ(model.eventOps[1].op, CudaEventOp::Record);
	EXPECT_EQ(model.eventOps[2].op, CudaEventOp::Synchronize);
	EXPECT_EQ(model.eventOps[3].op, CudaEventOp::Destroy);
}

// ── NvccStubDetector tests ────────────────────────────────────────────────────

TEST(NvccStubDetector, DetectsFatBinaryAndRegisterFunction) {
	auto fn = makeFnWithCalls("__cuda_module_init", {
			{"__cudaRegisterFatBinary", 0xf000},
			{"__cudaRegisterFunction", 0xf010},
	});

	CudaHostModel model;
	NvccStubDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_TRUE(model.hasCuda);
	ASSERT_EQ(model.kernelRegs.size(), 1u);
	EXPECT_EQ(model.kernelRegs[0].hostStubName, "__cuda_module_init");
}

// ── CudaHostRecovery (orchestrator) ───────────────────────────────────────────

TEST(CudaHostRecovery, AnalysesComplexModule) {
	SSAModule mod;

	{
		SSAFunction* fn = mod.addFunction("__cuda_init");
		fn->addBlock("entry");
		IrInstr* fat = fn->addInstr(0, IrInstr::Op::Call);
		fat->calleeName = "__cudaRegisterFatBinary";
		IrInstr* reg = fn->addInstr(0, IrInstr::Op::Call);
		reg->calleeName = "__cudaRegisterFunction";
	}

	{
		SSAFunction* fn = mod.addFunction("main");
		fn->addBlock("entry");
		IrInstr* setDev = fn->addInstr(0, IrInstr::Op::Call, 0x1000);
		setDev->calleeName = "cudaSetDevice";
		IrInstr* mal = fn->addInstr(0, IrInstr::Op::Call);
		mal->calleeName = "cudaMalloc";
		IrInstr* cpy = fn->addInstr(0, IrInstr::Op::Call);
		cpy->calleeName = "cudaMemcpy";
		IrInstr* launch = fn->addInstr(0, IrInstr::Op::Call);
		launch->calleeName = "cudaLaunchKernel";
		IrInstr* sync = fn->addInstr(0, IrInstr::Op::Call, 0x2000);
		sync->calleeName = "cudaDeviceSynchronize";
		IrInstr* fr = fn->addInstr(0, IrInstr::Op::Call, 0x2010);
		fr->calleeName = "cudaFree";
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
	SSAModule mod;
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
