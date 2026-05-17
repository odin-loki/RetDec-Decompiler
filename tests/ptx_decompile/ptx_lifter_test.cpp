/**
 * @file tests/ptx_decompile/ptx_lifter_test.cpp
 * @brief Unit tests for the PTX-to-CUDA-C Lifter.
 */

#include "retdec/ptx_decompile/ptx_lifter.h"
#include <gtest/gtest.h>
#include <string>

using namespace retdec::ptx_decompile;

// ─── ThreadIndexRecovery ──────────────────────────────────────────────────────

TEST(ThreadIndexRecovery, TidX) {
    EXPECT_EQ(ThreadIndexRecovery::resolve("%tid.x"), "threadIdx.x");
}
TEST(ThreadIndexRecovery, TidY) {
    EXPECT_EQ(ThreadIndexRecovery::resolve("%tid.y"), "threadIdx.y");
}
TEST(ThreadIndexRecovery, NtidX) {
    EXPECT_EQ(ThreadIndexRecovery::resolve("%ntid.x"), "blockDim.x");
}
TEST(ThreadIndexRecovery, CtaidX) {
    EXPECT_EQ(ThreadIndexRecovery::resolve("%ctaid.x"), "blockIdx.x");
}
TEST(ThreadIndexRecovery, NctaidX) {
    EXPECT_EQ(ThreadIndexRecovery::resolve("%nctaid.x"), "gridDim.x");
}
TEST(ThreadIndexRecovery, LaneId) {
    auto r = ThreadIndexRecovery::resolve("%laneid");
    EXPECT_NE(r.find("threadIdx.x"), std::string::npos);
    EXPECT_NE(r.find("31"), std::string::npos);
}
TEST(ThreadIndexRecovery, WarpId) {
    auto r = ThreadIndexRecovery::resolve("%warpid");
    EXPECT_NE(r.find("threadIdx.x"), std::string::npos);
    EXPECT_NE(r.find("5"), std::string::npos);  // >>5
}
TEST(ThreadIndexRecovery, Clock) {
    EXPECT_EQ(ThreadIndexRecovery::resolve("%clock"), "clock()");
}
TEST(ThreadIndexRecovery, IsSpecialTrue) {
    EXPECT_TRUE(ThreadIndexRecovery::isSpecial("%tid.x"));
    EXPECT_TRUE(ThreadIndexRecovery::isSpecial("%ctaid.z"));
}
TEST(ThreadIndexRecovery, IsSpecialFalse) {
    EXPECT_FALSE(ThreadIndexRecovery::isSpecial("%r5"));
    EXPECT_FALSE(ThreadIndexRecovery::isSpecial("myvar"));
}
TEST(ThreadIndexRecovery, Unknown) {
    EXPECT_TRUE(ThreadIndexRecovery::resolve("%unknown_reg").empty());
}

// ─── SharedMemDeclaration ─────────────────────────────────────────────────────

TEST(SharedMemDeclaration, BasicF32Array) {
    PtxVarDecl d;
    d.space = PtxSpace::Shared;
    d.type  = PtxType::F32;
    d.name  = "%smem";
    d.count = 1;
    d.bytes = 256 * 4;  // 256 floats
    auto s = SharedMemDeclaration::emit(d);
    EXPECT_NE(s.find("__shared__"), std::string::npos);
    EXPECT_NE(s.find("float"), std::string::npos);
    EXPECT_NE(s.find("smem"), std::string::npos);
}

TEST(SharedMemDeclaration, U32NoSize) {
    PtxVarDecl d;
    d.space = PtxSpace::Shared;
    d.type  = PtxType::U32;
    d.name  = "%sh";
    d.count = 16;
    auto s = SharedMemDeclaration::emit(d);
    EXPECT_NE(s.find("__shared__"), std::string::npos);
    EXPECT_NE(s.find("unsigned int"), std::string::npos);
    EXPECT_NE(s.find("[16]"), std::string::npos);
}

// ─── InstrLifter ─────────────────────────────────────────────────────────────

static PtxInstr makeInstr(const std::string& mn, PtxType ty,
                            std::vector<PtxOperand> ops,
                            const std::string& mod = "") {
    PtxInstr i;
    i.mnemonic = mn; i.type = ty; i.operands = std::move(ops); i.modifier = mod;
    return i;
}

static PtxOperand regOp(const std::string& name) {
    PtxOperand o; o.kind = PtxOperandKind::Register; o.name = "%" + name;
    return o;
}
static PtxOperand immOp(int64_t v) {
    PtxOperand o; o.kind = PtxOperandKind::Immediate; o.immVal = v;
    return o;
}
static PtxOperand specOp(const std::string& name) {
    PtxOperand o; o.kind = PtxOperandKind::SpecialReg; o.name = name;
    return o;
}
static PtxOperand addrOp(const std::string& base, int64_t off = 0) {
    PtxOperand o; o.kind = PtxOperandKind::Address;
    o.name = "%" + base; o.offset = off;
    return o;
}
static PtxOperand labelOp(const std::string& name) {
    PtxOperand o; o.kind = PtxOperandKind::Label; o.name = name;
    return o;
}

TEST(InstrLifter, Mov) {
    InstrLifter lft;
    auto i = makeInstr("mov", PtxType::U32, {regOp("r0"), specOp("%tid.x")});
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("r0"), std::string::npos);
    EXPECT_NE(s.find("threadIdx.x"), std::string::npos);
}

TEST(InstrLifter, AddU32) {
    InstrLifter lft;
    auto i = makeInstr("add", PtxType::U32, {regOp("r2"), regOp("r0"), regOp("r1")});
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("r2"), std::string::npos);
    EXPECT_NE(s.find("+"),  std::string::npos);
}

TEST(InstrLifter, MulU32) {
    InstrLifter lft;
    auto i = makeInstr("mul", PtxType::U32, {regOp("r3"), regOp("r1"), immOp(4)});
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("*"), std::string::npos);
    EXPECT_NE(s.find("4"), std::string::npos);
}

TEST(InstrLifter, LdGlobalF32) {
    InstrLifter lft;
    auto i = makeInstr("ld", PtxType::F32, {regOp("f0"), addrOp("rd0", 0)}, ".global.f32");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("f0"), std::string::npos);
    EXPECT_NE(s.find("*"),  std::string::npos);
}

TEST(InstrLifter, StGlobal) {
    InstrLifter lft;
    auto i = makeInstr("st", PtxType::F32, {addrOp("rd1", 8), regOp("f1")}, ".global.f32");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("f1"), std::string::npos);
}

TEST(InstrLifter, SetpLt) {
    InstrLifter lft;
    auto i = makeInstr("setp", PtxType::U32, {regOp("p0"), regOp("r0"), regOp("r1")}, ".u32.lt");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("p0"), std::string::npos);
    EXPECT_NE(s.find("<"),  std::string::npos);
}

TEST(InstrLifter, SelpTernary) {
    InstrLifter lft;
    auto i = makeInstr("selp", PtxType::F32,
                        {regOp("f0"), regOp("f1"), regOp("f2"), regOp("p0")});
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("?"), std::string::npos);
    EXPECT_NE(s.find(":"), std::string::npos);
}

TEST(InstrLifter, Cvt) {
    InstrLifter lft;
    auto i = makeInstr("cvt", PtxType::F32, {regOp("f0"), regOp("r0")}, ".f32.u32");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("float"), std::string::npos);
}

TEST(InstrLifter, BarSync) {
    InstrLifter lft;
    auto i = makeInstr("bar", PtxType::Unknown, {immOp(0)}, ".sync");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("__syncthreads"), std::string::npos);
}

TEST(InstrLifter, MembarGl) {
    InstrLifter lft;
    auto i = makeInstr("membar", PtxType::Unknown, {}, ".gl");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("__threadfence()"), std::string::npos);
}

TEST(InstrLifter, MembarCta) {
    InstrLifter lft;
    auto i = makeInstr("membar", PtxType::Unknown, {}, ".cta");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("__threadfence_block"), std::string::npos);
}

TEST(InstrLifter, AtomAdd) {
    InstrLifter lft;
    auto i = makeInstr("atom", PtxType::U32, {regOp("r0"), addrOp("rd0"), regOp("r1")}, ".global.add.u32");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("atomicAdd"), std::string::npos);
}

TEST(InstrLifter, AtomCas) {
    InstrLifter lft;
    auto i = makeInstr("atom", PtxType::U32, {regOp("r0"), addrOp("rd0"), regOp("r1"), regOp("r2")}, ".global.cas.b32");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("atomicCAS"), std::string::npos);
}

TEST(InstrLifter, VoteAll) {
    InstrLifter lft;
    auto i = makeInstr("vote", PtxType::Pred, {regOp("p0"), regOp("p1")}, ".all");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("__all_sync"), std::string::npos);
}

TEST(InstrLifter, ShflDown) {
    InstrLifter lft;
    auto i = makeInstr("shfl", PtxType::F32, {regOp("f1"), regOp("f0"), immOp(1)}, ".sync.down.b32");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("__shfl_down_sync"), std::string::npos);
}

TEST(InstrLifter, SqrtApprox) {
    InstrLifter lft;
    auto i = makeInstr("sqrt", PtxType::F32, {regOp("f0"), regOp("f1")}, ".approx.f32");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("sqrt"), std::string::npos);
}

TEST(InstrLifter, SinApprox) {
    InstrLifter lft;
    auto i = makeInstr("sin", PtxType::F32, {regOp("f0"), regOp("f1")}, ".approx.f32");
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("sin"), std::string::npos);
}

TEST(InstrLifter, BranchUnconditional) {
    InstrLifter lft;
    auto i = makeInstr("bra", PtxType::Unknown, {labelOp("BB1")});
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("goto"), std::string::npos);
    EXPECT_NE(s.find("BB1"), std::string::npos);
}

TEST(InstrLifter, BranchConditional) {
    InstrLifter lft;
    PtxInstr i;
    i.mnemonic = "bra";
    i.predReg  = "%p0";
    i.operands = {labelOp("DONE")};
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("if"), std::string::npos);
    EXPECT_NE(s.find("DONE"), std::string::npos);
}

TEST(InstrLifter, RetInstruction) {
    InstrLifter lft;
    auto i = makeInstr("ret", PtxType::Unknown, {});
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("return"), std::string::npos);
}

TEST(InstrLifter, LabelEmission) {
    InstrLifter lft;
    PtxInstr i;
    i.mnemonic = "__label__";
    i.label    = "LOOP";
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("LOOP:"), std::string::npos);
}

TEST(InstrLifter, UnknownAsComment) {
    InstrLifter lft;
    auto i = makeInstr("unknown_op", PtxType::Unknown, {});
    auto s = lft.lift(i, "");
    EXPECT_NE(s.find("/*"), std::string::npos);
    EXPECT_NE(s.find("unknown_op"), std::string::npos);
}

// ─── PtxParser ────────────────────────────────────────────────────────────────

static const char* kSimplePtx = R"PTX(
.version 7.5
.target sm_75
.address_size 64

.visible .entry vectorAdd(
    .param .u64 a,
    .param .u64 b,
    .param .u64 c,
    .param .u32 n
)
{
    .reg .u32  %r<4>;
    .reg .u64  %rd<4>;
    .reg .f32  %f<3>;
    .reg .pred %p<2>;

    ld.param.u64 %rd0, [a];
    ld.param.u64 %rd1, [b];
    ld.param.u64 %rd2, [c];
    ld.param.u32 %r0, [n];
    mov.u32 %r1, %tid.x;
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mad.lo.u32 %r3, %r2, %r3, %r1;
    setp.ge.u32 %p0, %r3, %r0;
    @%p0 bra END;
    ld.global.f32 %f0, [%rd0];
    ld.global.f32 %f1, [%rd1];
    add.f32 %f2, %f0, %f1;
    st.global.f32 [%rd2], %f2;
END:
    ret;
}
)PTX";

TEST(PtxParser, ParsesKernelName) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    ASSERT_FALSE(mod.kernels.empty());
    EXPECT_EQ(mod.kernels[0].name, "vectorAdd");
}

TEST(PtxParser, ParsesSmVersion) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    EXPECT_EQ(mod.smVersion, 75);
}

TEST(PtxParser, ParsesKernelParams) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    ASSERT_FALSE(mod.kernels.empty());
    const auto& k = mod.kernels[0];
    EXPECT_GE(k.params.size(), 4u);
}

TEST(PtxParser, ParsesRegDecls) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    ASSERT_FALSE(mod.kernels.empty());
    EXPECT_FALSE(mod.kernels[0].decls.empty());
}

TEST(PtxParser, ParsesInstructions) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    ASSERT_FALSE(mod.kernels.empty());
    EXPECT_GT(mod.kernels[0].instrs.size(), 5u);
}

TEST(PtxParser, EmptyInputEmptyModule) {
    PtxParser parser;
    auto mod = parser.parse("");
    EXPECT_TRUE(mod.kernels.empty());
}

// ─── PtxLifter ────────────────────────────────────────────────────────────────

TEST(PtxLifter, LiftSimplePtxContainsGlobal) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    PtxLifter lifter;
    auto s = lifter.liftModule(mod);
    EXPECT_NE(s.find("__global__"), std::string::npos);
}

TEST(PtxLifter, LiftContainsKernelName) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    PtxLifter lifter;
    auto s = lifter.liftModule(mod);
    EXPECT_NE(s.find("vectorAdd"), std::string::npos);
}

TEST(PtxLifter, LiftContainsSyncthreads_bar) {
    // A PTX with bar.sync
    const char* ptx = R"PTX(
.target sm_80
.visible .entry syncTest() {
    .reg .f32 %f<2>;
    bar.sync 0;
    ret;
}
)PTX";
    PtxParser parser;
    auto mod = parser.parse(ptx);
    PtxLifter lifter;
    auto s = lifter.liftKernel(mod.kernels[0]);
    EXPECT_NE(s.find("__syncthreads"), std::string::npos);
}

TEST(PtxLifter, LiftContainsThreadIdx) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    PtxLifter lifter;
    auto s = lifter.liftModule(mod);
    EXPECT_NE(s.find("threadIdx.x"), std::string::npos);
}

TEST(PtxLifter, LiftContainsBlockIdx) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    PtxLifter lifter;
    auto s = lifter.liftModule(mod);
    EXPECT_NE(s.find("blockIdx.x"), std::string::npos);
}

TEST(PtxLifter, LiftEmptyKernelNocrash) {
    PtxKernel k;
    k.name = "empty_kernel";
    k.kind = PtxKernelKind::Entry;
    PtxLifter lifter;
    auto s = lifter.liftKernel(k);
    EXPECT_NE(s.find("empty_kernel"), std::string::npos);
    EXPECT_NE(s.find("{"), std::string::npos);
    EXPECT_NE(s.find("}"), std::string::npos);
}

TEST(PtxLifter, LiftDeviceFunction) {
    const char* ptx = R"PTX(
.target sm_75
.visible .func add_func(.param .u32 x, .param .u32 y) {
    .reg .u32 %r<3>;
    ld.param.u32 %r0, [x];
    ld.param.u32 %r1, [y];
    add.u32 %r2, %r0, %r1;
    ret;
}
)PTX";
    PtxParser parser;
    auto mod = parser.parse(ptx);
    PtxLifter lifter;
    auto s = lifter.liftModule(mod);
    EXPECT_NE(s.find("__device__"), std::string::npos);
    EXPECT_NE(s.find("add_func"), std::string::npos);
}

TEST(PtxLifter, LiftContainsCudaRuntimeInclude) {
    PtxParser parser;
    auto mod = parser.parse(kSimplePtx);
    PtxLifter lifter;
    auto s = lifter.liftModule(mod);
    EXPECT_NE(s.find("#include <cuda_runtime.h>"), std::string::npos);
}

TEST(PtxLifter, LiftSharedMemDecl) {
    const char* ptx = R"PTX(
.target sm_75
.visible .entry sharedTest() {
    .shared .align 4 .f32 %smem[256];
    .reg .u32 %r<2>;
    bar.sync 0;
    ret;
}
)PTX";
    PtxParser parser;
    auto mod = parser.parse(ptx);
    PtxLifter lifter;
    auto s = lifter.liftKernel(mod.kernels[0]);
    EXPECT_NE(s.find("__shared__"), std::string::npos);
}

TEST(PtxLifter, LiftAtomicAdd) {
    const char* ptx = R"PTX(
.target sm_75
.visible .entry atomicTest(.param .u64 ptr) {
    .reg .u64 %rd<2>;
    .reg .u32 %r<3>;
    ld.param.u64 %rd0, [ptr];
    mov.u32 %r0, 1;
    atom.global.add.u32 %r1, [%rd0], %r0;
    ret;
}
)PTX";
    PtxParser parser;
    auto mod = parser.parse(ptx);
    PtxLifter lifter;
    auto s = lifter.liftKernel(mod.kernels[0]);
    EXPECT_NE(s.find("atomicAdd"), std::string::npos);
}
