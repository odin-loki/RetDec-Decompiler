/**
 * @file tests/cil_reconstruct/cil_reconstruct_test.cpp
 * @brief Unit tests for the CIL reconstruction pipeline.
 */

#include <memory>
#include "retdec/cil_reconstruct/cil_patterns.h"
#include "retdec/cil_reconstruct/cil_reconstructor.h"
#include "retdec/cil_reconstruct/cil_stack_sim.h"
#include "retdec/cil_reconstruct/cil_var_recovery.h"

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/bc_module/bc_type.h"

#include <gtest/gtest.h>

using namespace retdec::bc_module;
using namespace retdec::cil_reconstruct;

// ─── Helpers ──────────────────────────────────────────────────────────────────

/**
 * @brief Build a trivial BcMethod with manually constructed BcCFG.
 */
static BcMethod makeMethod(const std::string& name, bool isVoid = true) {
    BcMethod m;
    m.name = name;
    m.descriptor.returnType = isVoid ? std::make_shared<BcType>(types::Void())
                                     : std::make_shared<BcType>(types::Int());
    return m;
}

/**
 * @brief Add a basic block to a cfg and return a reference to it.
 */
static BcBasicBlock& addBlock(BcCFG& cfg) {
    return cfg.addBlock();
}

/**
 * @brief Create a simple instruction with the given opcode.
 */
static BcInstruction makeInsn(BcOpcode op) {
    BcInstruction i;
    i.opcode = op;
    return i;
}

static BcInstruction makeInsnInt(BcOpcode op, int64_t v) {
    BcInstruction i;
    i.opcode = op;
    i.operands.push_back(BcIntOperand{v});
    return i;
}

static BcInstruction makeInsnLocal(BcOpcode op, uint32_t idx) {
    BcInstruction i;
    i.opcode = op;
    i.operands.push_back(BcLocalOperand{idx});
    return i;
}

// ─── Stack simulation tests ───────────────────────────────────────────────────

TEST(CilStackSim, EmptyMethod) {
    BcMethod m = makeMethod("Empty");
    // No blocks
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    EXPECT_TRUE(sim.isValid());
}

TEST(CilStackSim, LdcI4PushesInt) {
    BcMethod m = makeMethod("LdcTest");
    BcBasicBlock& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsnInt(BcOpcode::DOTNET_LDC_I4, 42));

    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    EXPECT_TRUE(sim.isValid());

    const auto& st = sim.instrStack(0, 0);
    ASSERT_EQ(1u, st.size());
    EXPECT_TRUE(st[0].type == types::Int());

    auto expr = sim.exprAt(0, 0);
    ASSERT_NE(nullptr, expr);
    ASSERT_TRUE(expr->isConst());
    EXPECT_EQ(42, expr->asConst().intVal);
}

TEST(CilStackSim, LdcI4_0Through8) {
    for (int v = 0; v <= 8; ++v) {
        BcOpcode op = static_cast<BcOpcode>(
            static_cast<int>(BcOpcode::DOTNET_LDC_I4_0) + v);
        BcMethod m = makeMethod("Ldc" + std::to_string(v));
        auto& b0 = m.cfg.addBlock();
        b0.instrs.push_back(makeInsn(op));
        CilStackSimulator sim;
        EXPECT_TRUE(sim.simulate(m.cfg, m));
        EXPECT_EQ(1u, sim.instrStack(0, 0).size());
    }
}

TEST(CilStackSim, LdcI4_M1) {
    BcMethod m = makeMethod("LdcM1");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_M1));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    auto expr = sim.exprAt(0, 0);
    ASSERT_NE(nullptr, expr);
    EXPECT_EQ(-1, expr->asConst().intVal);
}

TEST(CilStackSim, LdnullPushesNull) {
    BcMethod m = makeMethod("LdnullTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDNULL));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    const auto& st = sim.instrStack(0, 0);
    ASSERT_EQ(1u, st.size());
    auto expr = sim.exprAt(0, 0);
    ASSERT_NE(nullptr, expr);
    EXPECT_TRUE(expr->isNull());
}

TEST(CilStackSim, LdstrPushesString) {
    BcMethod m = makeMethod("LdstrTest");
    auto& b0 = m.cfg.addBlock();
    BcInstruction insn;
    insn.opcode = BcOpcode::DOTNET_LDSTR;
    insn.operands.push_back(BcStringOperand{"hello"});
    b0.instrs.push_back(std::move(insn));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    const auto& st = sim.instrStack(0, 0);
    ASSERT_EQ(1u, st.size());
    EXPECT_EQ(types::ClrString(), st[0].type);
}

TEST(CilStackSim, BinaryAddReducesStack) {
    BcMethod m = makeMethod("AddTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_2));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_ADD));

    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    // After ADD: stack should have 1 element
    EXPECT_EQ(1u, sim.instrStack(0, 2).size());
    auto expr = sim.exprAt(0, 2);
    ASSERT_NE(nullptr, expr);
    ASSERT_TRUE(expr->isBinOp());
    EXPECT_EQ(BinOpKind::Add, expr->asBinOp().op);
}

TEST(CilStackSim, NopDoesNotChangeStack) {
    BcMethod m = makeMethod("NopTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    EXPECT_EQ(0u, sim.instrStack(0, 0).size());
}

TEST(CilStackSim, DupDuplicatesTop) {
    BcMethod m = makeMethod("DupTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_DUP));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    EXPECT_EQ(2u, sim.instrStack(0, 1).size());
}

TEST(CilStackSim, PopReducesStack) {
    BcMethod m = makeMethod("PopTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_POP));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    EXPECT_EQ(0u, sim.instrStack(0, 1).size());
}

TEST(CilStackSim, StlocClearsStack) {
    BcMethod m = makeMethod("StlocTest");
    m.locals.push_back(BcLocalVar{0, "x", types::Int()});
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_5));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_STLOC_0));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    EXPECT_EQ(0u, sim.instrStack(0, 1).size());
}

TEST(CilStackSim, LdlocPushesLocalType) {
    BcMethod m = makeMethod("LdlocTest");
    m.locals.push_back(BcLocalVar{0, "x", types::Int()});
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDLOC_0));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    ASSERT_EQ(1u, sim.instrStack(0, 0).size());
    EXPECT_EQ(types::Int(), sim.instrStack(0, 0)[0].type);
    auto expr = sim.exprAt(0, 0);
    ASSERT_NE(nullptr, expr);
    ASSERT_TRUE(expr->isLocal());
    EXPECT_EQ(0u, expr->asLocal().idx);
    EXPECT_EQ("x", expr->asLocal().name);
}

TEST(CilStackSim, ConvI4ChangesType) {
    BcMethod m = makeMethod("ConvTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_CONV_I8));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    ASSERT_EQ(1u, sim.instrStack(0, 1).size());
    EXPECT_EQ(types::Long(), sim.instrStack(0, 1)[0].type);
}

TEST(CilStackSim, CeqProducesBool) {
    BcMethod m = makeMethod("CeqTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_CEQ));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    ASSERT_EQ(1u, sim.instrStack(0, 2).size());
    EXPECT_EQ(types::Bool(), sim.instrStack(0, 2)[0].type);
}

TEST(CilStackSim, NewarrPushesArrayType) {
    BcMethod m = makeMethod("NewarrTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_5));  // length
    BcInstruction na;
    na.opcode = BcOpcode::DOTNET_NEWARR;
    na.operands.push_back(BcTypeOperand{types::Int()});
    b0.instrs.push_back(std::move(na));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    ASSERT_EQ(1u, sim.instrStack(0, 1).size());
    EXPECT_TRUE(sim.instrStack(0, 1)[0].type.isArray());
}

TEST(CilStackSim, BoxPushesObject) {
    BcMethod m = makeMethod("BoxTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    BcInstruction box;
    box.opcode = BcOpcode::DOTNET_BOX;
    box.operands.push_back(BcTypeOperand{types::Int()});
    b0.instrs.push_back(std::move(box));
    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    ASSERT_EQ(1u, sim.instrStack(0, 1).size());
    EXPECT_TRUE(sim.instrStack(0, 1)[0].type.isClass());
}

// ─── Multi-block fixpoint tests ───────────────────────────────────────────────

TEST(CilStackSim, TwoBlockLinear) {
    BcMethod m = makeMethod("TwoBlock");
    auto& b0 = m.cfg.addBlock();
    auto& b1 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_2));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_ADD));
    // Goto b1
    BcInstruction br;
    br.opcode = BcOpcode::DOTNET_BR;
    br.operands.push_back(BcBlockOperand{1});
    b0.instrs.push_back(std::move(br));
    b0.succs.push_back(1);
    b1.preds.push_back(0);
    b1.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));

    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    // b1 entry stack should inherit from b0 (1 int on stack)
    // But BR clears nothing; the propagated stack has 1 element
    EXPECT_EQ(1u, sim.entryStack(1).size());
}

TEST(CilStackSim, ConditionalBranch) {
    BcMethod m = makeMethod("CondBranch");
    auto& b0 = m.cfg.addBlock();
    auto& b1 = m.cfg.addBlock();
    auto& b2 = m.cfg.addBlock();
    // Push 1, brfalse to b2, else fall through to b1
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    BcInstruction brfalse;
    brfalse.opcode = BcOpcode::DOTNET_BRFALSE;
    brfalse.operands.push_back(BcBlockOperand{2});
    b0.instrs.push_back(std::move(brfalse));
    b0.succs = {1, 2};
    b1.preds = {0}; b2.preds = {0};
    b1.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));
    b2.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));

    CilStackSimulator sim;
    EXPECT_TRUE(sim.simulate(m.cfg, m));
    // After brfalse, stack is empty at both successors
    EXPECT_EQ(0u, sim.entryStack(1).size());
    EXPECT_EQ(0u, sim.entryStack(2).size());

    // And they were actually analysed. An empty entry stack is also what a
    // block that was never visited has, so the two assertions above held for
    // an implementation that walked neither successor -- which is what
    // runFixpoint did.
    EXPECT_EQ(b1.instrs.size(), sim.instrStackCount(1));
    EXPECT_EQ(b2.instrs.size(), sim.instrStackCount(2));
}

// ─── The work-list ──────────────────────────────────────────────────────────
//
// runFixpoint() enqueued a successor only when the meet CHANGED its recorded
// entry stack, and there was no visited bit. An entry stack starts out
// default-constructed, which is empty, and meetStates(empty, empty) is empty --
// so the comparison was false. In CIL the evaluation stack is empty at almost
// every block boundary: it is required to be at every `leave`, and compiler
// output empties it before each branch. Every block but the entry was
// therefore left unanalysed, with instrStacks empty and exprAt() returning
// nullptr for every instruction in it, while simulate() returned true.
//
// Measured on the chain below: 1 of 2 blocks analysed, and 1 of 40.

namespace {

/// A chain of @a n blocks, each pushing a constant and storing it -- so every
/// block's exit stack is EMPTY, which is what CIL requires at a boundary.
BcMethod makeEmptyStackChain(int n) {
    BcMethod m = makeMethod("chain");
    for (int i = 0; i < n; ++i) {
        auto& b = m.cfg.addBlock();
        b.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
        b.instrs.push_back(makeInsnLocal(BcOpcode::DOTNET_STLOC, static_cast<uint32_t>(i)));
        if (i + 1 < n) {
            BcInstruction br;
            br.opcode = BcOpcode::DOTNET_BR;
            br.operands.push_back(BcBlockOperand{static_cast<uint32_t>(i + 1)});
            b.instrs.push_back(std::move(br));
            b.succs.push_back(i + 1);
        }
    }
    for (int i = 1; i < n; ++i) m.cfg.block(i).preds.push_back(i - 1);
    return m;
}

} // namespace

TEST(CilStackSim, EverySuccessorIsAnalysedEvenWhenTheStackIsEmpty) {
    BcMethod m = makeEmptyStackChain(2);

    CilStackSimulator sim;
    ASSERT_TRUE(sim.simulate(m.cfg, m)) << sim.error();

    // Both blocks, not just the entry.
    EXPECT_EQ(3u, sim.instrStackCount(0));
    EXPECT_EQ(2u, sim.instrStackCount(1));

    // And the expressions are there, which is what CilVarRecovery reads. The
    // second block's ldc came back <null> before the fix, so its local was
    // declared with no initialiser.
    EXPECT_NE(nullptr, sim.exprAt(0, 0));
    EXPECT_NE(nullptr, sim.exprAt(1, 0));
}

TEST(CilStackSim, ALongChainIsAnalysedToTheEnd) {
    // maxIterations was a cap on blocks POPPED, not on revisits per block, and
    // it defaults to 32 -- so a method with more than 32 basic blocks, which is
    // routine, had the rest left default-constructed while runFixpoint returned
    // true. Measured with the visited fix alone: 32 of 40.
    const int kBlocks = 40;
    BcMethod m = makeEmptyStackChain(kBlocks);

    CilStackSimulator sim;
    ASSERT_TRUE(sim.simulate(m.cfg, m)) << sim.error();

    for (int i = 0; i < kBlocks; ++i) {
        EXPECT_GT(sim.instrStackCount(static_cast<uint32_t>(i)), 0u)
            << "block " << i << " was never analysed";
    }
}

TEST(CilStackSim, GivingUpIsReportedRatherThanReturnedAsSuccess) {
    // The cap is a backstop rather than something a well-formed CFG reaches:
    // meetStates widens monotonically and keeps the existing state on a depth
    // mismatch, so a block's entry stack settles after a visit or two and 32 is
    // generous. What matters is that hitting it is REPORTED -- it used to
    // return true with the remaining blocks left default-constructed. Driven
    // here with a budget of zero visits, which is the one setting that reaches
    // the path from any input.
    BcMethod m = makeEmptyStackChain(3);

    CilStackSimulator::Options opts;
    opts.maxIterations = 0;
    CilStackSimulator sim(opts);

    EXPECT_FALSE(sim.simulate(m.cfg, m));
    EXPECT_FALSE(sim.isValid());
    EXPECT_NE(std::string::npos, sim.error().find("did not converge")) << sim.error();

    // And the default budget is enough for the same method.
    CilStackSimulator ok;
    EXPECT_TRUE(ok.simulate(m.cfg, m)) << ok.error();
    EXPECT_GT(ok.instrStackCount(2), 0u);
}

// ─── Instance-call operand order ────────────────────────────────────────────
//
// ECMA-335 III.3.19 and III.4.2: for an instance call the object reference is
// pushed BEFORE arg1..argN, so it is the deepest slot and argN is on top. The
// handler popped the top first as the receiver and then popN()'d the rest --
// which still included the real receiver at index 0 -- so obj was argN and args
// was [obj, arg1, .., argN-1]. Only a zero-argument instance call came out
// right. Measured on `ldarg.0; ldarg.1; callvirt Append(int)`:
// arg1.Append(arg0) instead of arg0.Append(arg1).

namespace {

/// The argument index of an ExprArg, or -1.
int argIndexOf(const CilExprPtr& e) {
    if (!e) return -1;
    const auto* a = std::get_if<ExprArg>(&e->data);
    return a ? static_cast<int>(a->idx) : -1;
}

/// A method with @a nparams reference/int parameters, one block that loads
/// ldarg.0 .. ldarg.N and then calls `Target::M` with @a nargs of them.
BcMethod makeInstanceCall(BcOpcode callOp, int nargs) {
    BcMethod m = makeMethod("caller");
    m.descriptor.params.push_back(std::make_shared<BcType>(types::ClrObject()));
    for (int i = 0; i < nargs; ++i)
        m.descriptor.params.push_back(std::make_shared<BcType>(types::Int()));

    auto& b = m.cfg.addBlock();
    for (int i = 0; i <= nargs; ++i)
        b.instrs.push_back(makeInsnLocal(BcOpcode::DOTNET_LDARG, static_cast<uint32_t>(i)));

    BcInstruction call;
    call.opcode = callOp;
    BcMethodRef mr;
    mr.owner = "Target";
    mr.name = "M";
    mr.descriptor.returnType = std::make_shared<BcType>(types::ClrObject());
    for (int i = 0; i < nargs; ++i)
        mr.descriptor.params.push_back(std::make_shared<BcType>(types::Int()));
    call.operands.push_back(mr);
    b.instrs.push_back(std::move(call));
    return m;
}

} // namespace

TEST(CilStackSim, InstanceCallTakesTheDeepestSlotAsItsReceiver) {
    for (int nargs = 1; nargs <= 3; ++nargs) {
        BcMethod m = makeInstanceCall(BcOpcode::DOTNET_CALLVIRT, nargs);

        CilStackSimulator sim;
        ASSERT_TRUE(sim.simulate(m.cfg, m)) << sim.error();

        auto e = sim.exprAt(0, static_cast<uint32_t>(nargs + 1));
        ASSERT_NE(nullptr, e) << "nargs=" << nargs;
        const auto* call = std::get_if<ExprCall>(&e->data);
        ASSERT_NE(nullptr, call) << "nargs=" << nargs;

        // ldarg.0 is the receiver; ldarg.1..N are the arguments, in order.
        EXPECT_EQ(0, argIndexOf(call->obj)) << "nargs=" << nargs;
        ASSERT_EQ(static_cast<size_t>(nargs), call->args.size()) << "nargs=" << nargs;
        for (int i = 0; i < nargs; ++i) {
            EXPECT_EQ(i + 1, argIndexOf(call->args[static_cast<size_t>(i)]))
                << "nargs=" << nargs << " arg " << i;
        }
    }
}

// A zero-argument instance call is the one shape that was already right, and
// has to stay right.
TEST(CilStackSim, AZeroArgumentInstanceCallKeepsItsReceiver) {
    BcMethod m = makeInstanceCall(BcOpcode::DOTNET_CALLVIRT, 0);

    CilStackSimulator sim;
    ASSERT_TRUE(sim.simulate(m.cfg, m)) << sim.error();

    auto e = sim.exprAt(0, 1);
    ASSERT_NE(nullptr, e);
    const auto* call = std::get_if<ExprCall>(&e->data);
    ASSERT_NE(nullptr, call);
    EXPECT_EQ(0, argIndexOf(call->obj));
    EXPECT_TRUE(call->args.empty());
}

// The if/else path used to be gated on `opts_.structureExcept == false` --
// not a predicate about if/else at all, and false for every default-constructed
// option set, so buildIfElse was reachable only by turning exception
// structuring off. It has its own option now, defaulted to today's behaviour
// because buildIfElse does not yet nest (see the comment on structureIf).

TEST(CilReconstructor, TheIfElsePathIsGatedOnItsOwnOption) {
    BcMethod m = makeMethod("cond");
    auto& b0 = m.cfg.addBlock();
    auto& b1 = m.cfg.addBlock();
    auto& b2 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    BcInstruction brtrue;
    brtrue.opcode = BcOpcode::DOTNET_BRTRUE;
    brtrue.operands.push_back(BcBlockOperand{2});
    b0.instrs.push_back(std::move(brtrue));
    b0.succs = {1, 2};
    b1.preds = {0};
    b2.preds = {0};
    b1.instrs.push_back(makeInsn(BcOpcode::DOTNET_RET));
    b2.instrs.push_back(makeInsn(BcOpcode::DOTNET_RET));

    BcModule mod;

    // Exception structuring must not decide this either way.
    CilReconstructOptions withEh;
    withEh.structureExcept = true;
    CilReconstructOptions withoutEh;
    withoutEh.structureExcept = false;

    const auto a = CilReconstructor(withEh).reconstruct(m, mod);
    const auto b = CilReconstructor(withoutEh).reconstruct(m, mod);
    EXPECT_EQ(a.method.body.size(), b.method.body.size())
        << "structureExcept changed the shape of a method with no handlers";

    // And the option that does control it, does.
    CilReconstructOptions ifOn;
    ifOn.structureIf = true;
    const auto c = CilReconstructor(ifOn).reconstruct(m, mod);
    EXPECT_NE(a.method.body.size(), c.method.body.size())
        << "structureIf did not reach buildIfElse";
}

// ─── The `using` rewrite ────────────────────────────────────────────────────
//
// Two defects in detectUsingStatements, both of which the recovered `using`
// carried into the emitted C#.
//
//   * The "skip the preceding LocalDecl" guard tested newBody.back() AFTER
//     pushing the Using, so it tested the Using itself and the pop_back was
//     dead. The declaration stayed beside the `using` that re-introduces the
//     same resource.
//   * matchUsingPattern read stmts[pos-1] -- which the loop had already moved
//     into newBody. A moved-from CilStmt keeps its `kind` (a plain enum) and
//     loses its `target` and `expr` (shared_ptrs), so the declaration matched
//     and then yielded nothing: every recovered `using` had an empty variable
//     name and a null initialiser.
//
// Measured before: body.size() = 2, [0] LocalDecl, [1] Using with
// iterVarName='' and expr=<null>.

namespace {

CilExprPtr makeLocalExpr(const char* name) {
    ExprLocal l;
    l.name = name;
    l.type = types::ClrObject();
    return std::make_shared<CilExpr>(std::move(l), types::ClrObject());
}

CilExprPtr makeCallExpr(const char* cls, const char* meth, CilExprPtr obj) {
    ExprCall c;
    c.className  = cls;
    c.methodName = meth;
    c.obj        = std::move(obj);
    c.retType    = types::Void();
    return std::make_shared<CilExpr>(std::move(c), types::Void());
}

/// `FileStream fs = new FileStream(); try { fs.Read(); } finally { fs.Dispose(); }`
CilRecoveredMethod makeUsingShape() {
    CilRecoveredMethod m;

    CilStmt decl;
    decl.kind   = StmtKind::LocalDecl;
    decl.target = makeLocalExpr("fs");
    decl.expr   = makeCallExpr("System.IO.FileStream", ".ctor", nullptr);
    m.body.push_back(std::move(decl));

    CilStmt tryStmt;
    tryStmt.kind = StmtKind::Try;
    CilStmt inner;
    inner.kind = StmtKind::ExprStmt;
    inner.expr = makeCallExpr("System.IO.FileStream", "Read", makeLocalExpr("fs"));
    tryStmt.tryBody.push_back(std::move(inner));
    CilStmt dispose;
    dispose.kind = StmtKind::ExprStmt;
    dispose.expr = makeCallExpr("System.IO.FileStream", "Dispose", makeLocalExpr("fs"));
    tryStmt.finallyBody.push_back(std::move(dispose));
    m.body.push_back(std::move(tryStmt));

    return m;
}

} // namespace

TEST(CilPatterns, AUsingAbsorbsItsDeclarationAndKeepsItsName) {
    CilRecoveredMethod m = makeUsingShape();
    BcModule mod;

    CilPatternDetector().detect(m, mod);

    ASSERT_EQ(1u, m.body.size()) << "the declaration was left beside the using";
    EXPECT_EQ(StmtKind::Using, m.body[0].kind);
    EXPECT_EQ("fs", m.body[0].iterVarName);
    ASSERT_NE(nullptr, m.body[0].expr) << "the initialiser was lost";
    EXPECT_TRUE(m.body[0].expr->isCall());
    EXPECT_EQ(".ctor", m.body[0].expr->asCall().methodName);
}

// A try/finally/Dispose with no declaration before it is still a `using`; it
// just has nothing to absorb, and must not eat whatever does precede it.
TEST(CilPatterns, AUsingWithNoDeclarationEatsNothing) {
    CilRecoveredMethod m = makeUsingShape();

    // Put an unrelated statement where the declaration was.
    m.body[0] = CilStmt{};
    m.body[0].kind = StmtKind::ExprStmt;
    m.body[0].expr = makeCallExpr("Other", "Unrelated", nullptr);

    BcModule mod;
    CilPatternDetector().detect(m, mod);

    ASSERT_EQ(2u, m.body.size());
    EXPECT_EQ(StmtKind::ExprStmt, m.body[0].kind);
    EXPECT_EQ(StmtKind::Using, m.body[1].kind);
    EXPECT_TRUE(m.body[1].iterVarName.empty());
}

// ─── Several catches over one region ────────────────────────────────────────
//
// ECMA-335 encodes `try { } catch (A) { } catch (B) { }` as TWO EH-table
// entries over the SAME protected region. structureEH looped once per entry
// and each iteration drained every non-EH block into that iteration's tryBody,
// so the first handler swallowed the whole region and the second produced a
// second Try with an EMPTY body and a live catch -- two sibling try statements
// rather than one try with two catches, which stops catching B around the body
// at all. Measured before: [0] Try tryBody=3 catches=1, [1] Try tryBody=0
// catches=1.

TEST(CilReconstructor, TwoCatchesOverOneRegionBecomeOneTry) {
    BcModule module("EH", SourceLang::CSharp);
    BcClass& cls = module.addClass(BcClass{});
    cls.name = "EHTest";
    cls.fqName = "EHTest";

    BcMethod& m = cls.methods.emplace_back();
    m.name = "TwoCatches";
    m.descriptor.returnType = std::make_shared<BcType>(types::Void());

    auto& b0 = m.cfg.addBlock(); // the protected region
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    b0.instrs.push_back(makeInsnLocal(BcOpcode::DOTNET_STLOC, 0));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LEAVE));

    auto& b1 = m.cfg.addBlock(); // catch ExA
    b1.isExceptionHandler = true;
    b1.instrs.push_back(makeInsn(BcOpcode::DOTNET_LEAVE));

    auto& b2 = m.cfg.addBlock(); // catch ExB
    b2.isExceptionHandler = true;
    b2.instrs.push_back(makeInsn(BcOpcode::DOTNET_LEAVE));

    auto& b3 = m.cfg.addBlock();
    b3.instrs.push_back(makeInsn(BcOpcode::DOTNET_RET));

    BcExceptionHandler a;
    a.startOffset = 0;
    a.endOffset = 3;
    a.handlerBlock = 1;
    a.catchType = types::Class("ExA");
    m.cfg.addExceptionHandler(a);

    BcExceptionHandler b;
    b.startOffset = 0;
    b.endOffset = 3;
    b.handlerBlock = 2;
    b.catchType = types::Class("ExB");
    m.cfg.addExceptionHandler(b);

    CilReconstructor rec;
    auto result = rec.reconstruct(m, module);

    size_t tries = 0;
    const CilStmt* first = nullptr;
    for (const auto& st : result.method.body) {
        if (st.kind == StmtKind::Try) {
            ++tries;
            if (first == nullptr) first = &st;
        }
    }

    ASSERT_EQ(1u, tries) << "one protected region, one try";
    ASSERT_NE(nullptr, first);
    EXPECT_EQ(2u, first->catches.size());
    EXPECT_FALSE(first->tryBody.empty()) << "the try body must not be empty";
}

// Two regions are still two tries: the grouping is by protected region, not a
// blanket merge of every handler in the method.
TEST(CilReconstructor, TwoDistinctRegionsStayTwoTries) {
    BcModule module("EH", SourceLang::CSharp);
    BcClass& cls = module.addClass(BcClass{});
    cls.name = "EHTest";
    cls.fqName = "EHTest";

    BcMethod& m = cls.methods.emplace_back();
    m.name = "TwoRegions";
    m.descriptor.returnType = std::make_shared<BcType>(types::Void());

    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LEAVE));
    auto& b1 = m.cfg.addBlock();
    b1.isExceptionHandler = true;
    b1.instrs.push_back(makeInsn(BcOpcode::DOTNET_LEAVE));
    auto& b2 = m.cfg.addBlock();
    b2.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));
    b2.instrs.push_back(makeInsn(BcOpcode::DOTNET_LEAVE));
    auto& b3 = m.cfg.addBlock();
    b3.isExceptionHandler = true;
    b3.instrs.push_back(makeInsn(BcOpcode::DOTNET_LEAVE));

    BcExceptionHandler a;
    a.startOffset = 0;
    a.endOffset = 2;
    a.handlerBlock = 1;
    a.catchType = types::Class("ExA");
    m.cfg.addExceptionHandler(a);

    BcExceptionHandler b;
    b.startOffset = 4;
    b.endOffset = 6;
    b.handlerBlock = 3;
    b.catchType = types::Class("ExB");
    m.cfg.addExceptionHandler(b);

    CilReconstructor rec;
    auto result = rec.reconstruct(m, module);

    size_t tries = 0;
    for (const auto& st : result.method.body)
        if (st.kind == StmtKind::Try) ++tries;
    EXPECT_EQ(2u, tries);
}

// ─── Variable recovery tests ─────────────────────────────────────────────────

TEST(CilVarRecovery, BuildsLocalVarTable) {
    BcMethod m = makeMethod("VarTest");
    m.locals.push_back(BcLocalVar{0, "count", types::Int()});
    m.locals.push_back(BcLocalVar{1, "name",  types::ClrString()});
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));

    CilStackSimulator sim;
    sim.simulate(m.cfg, m);

    CilVarRecovery recovery;
    auto result = recovery.recover(m.cfg, m, sim);
    ASSERT_EQ(2u, result.locals.size());
    EXPECT_EQ("count", result.locals[0].name);
    EXPECT_EQ("name",  result.locals[1].name);
    EXPECT_EQ(types::Int(), result.locals[0].type);
}

TEST(CilVarRecovery, GeneratesDefaultLocalNames) {
    BcMethod m = makeMethod("NoNameVars");
    m.locals.push_back(BcLocalVar{0, "", types::Int()});
    m.locals.push_back(BcLocalVar{1, "", types::Double()});
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));

    CilStackSimulator sim;
    sim.simulate(m.cfg, m);
    CilVarRecovery recovery;
    auto result = recovery.recover(m.cfg, m, sim);
    ASSERT_EQ(2u, result.locals.size());
    EXPECT_EQ("loc0", result.locals[0].name);
    EXPECT_EQ("loc1", result.locals[1].name);
}

TEST(CilVarRecovery, BuildsParamTable) {
    BcMethod m = makeMethod("ParamTest");
    m.paramNames = {"x", "y"};
    m.descriptor.params.push_back(std::make_shared<BcType>(types::Int()));
    m.descriptor.params.push_back(std::make_shared<BcType>(types::Double()));
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));

    CilStackSimulator sim;
    sim.simulate(m.cfg, m);
    CilVarRecovery recovery;
    auto result = recovery.recover(m.cfg, m, sim);
    ASSERT_EQ(2u, result.params.size());
    EXPECT_EQ("x", result.params[0].name);
    EXPECT_EQ("y", result.params[1].name);
}

TEST(CilVarRecovery, StoreProducesAssignStmt) {
    BcMethod m = makeMethod("StoreTest");
    m.locals.push_back(BcLocalVar{0, "n", types::Int()});
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_5));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_STLOC_0));

    CilStackSimulator sim;
    sim.simulate(m.cfg, m);
    CilVarRecovery recovery;
    auto result = recovery.recover(m.cfg, m, sim);
    ASSERT_EQ(1u, result.blocks.size());

    // Should have at least one statement (LocalDecl or Assign for the store)
    bool hasAssign = false;
    for (const auto& s : result.blocks[0].stmts) {
        if (s.kind == StmtKind::LocalDecl || s.kind == StmtKind::Assign) {
            hasAssign = true;
            break;
        }
    }
    EXPECT_TRUE(hasAssign);
}

TEST(CilVarRecovery, ReturnProducesReturnStmt) {
    BcMethod m = makeMethod("RetTest", false); // non-void
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_0));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_RET));

    CilStackSimulator sim;
    sim.simulate(m.cfg, m);
    CilVarRecovery recovery;
    auto result = recovery.recover(m.cfg, m, sim);
    ASSERT_EQ(1u, result.blocks.size());

    bool hasReturn = false;
    for (const auto& s : result.blocks[0].stmts) {
        if (s.kind == StmtKind::Return) { hasReturn = true; break; }
    }
    EXPECT_TRUE(hasReturn);
}

TEST(CilVarRecovery, ThrowProducesThrowStmt) {
    BcMethod m = makeMethod("ThrowTest");
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDNULL));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_THROW));

    CilStackSimulator sim;
    sim.simulate(m.cfg, m);
    CilVarRecovery recovery;
    auto result = recovery.recover(m.cfg, m, sim);
    ASSERT_EQ(1u, result.blocks.size());
    bool hasThrow = false;
    for (const auto& s : result.blocks[0].stmts) {
        if (s.kind == StmtKind::Throw) { hasThrow = true; break; }
    }
    EXPECT_TRUE(hasThrow);
}

TEST(CilVarRecovery, BranchProducesGotoOrIf) {
    BcMethod m = makeMethod("BranchTest");
    auto& b0 = m.cfg.addBlock();
    auto& b1 = m.cfg.addBlock();
    BcInstruction br;
    br.opcode = BcOpcode::DOTNET_BR;
    br.operands.push_back(BcBlockOperand{1});
    b0.instrs.push_back(std::move(br));
    b0.succs = {1};
    b1.preds = {0};
    b1.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));

    CilStackSimulator sim;
    sim.simulate(m.cfg, m);
    CilVarRecovery recovery;
    auto result = recovery.recover(m.cfg, m, sim);
    ASSERT_EQ(2u, result.blocks.size());
    bool hasGoto = false;
    for (const auto& s : result.blocks[0].stmts) {
        if (s.kind == StmtKind::Goto) { hasGoto = true; break; }
    }
    EXPECT_TRUE(hasGoto);
}

TEST(CilVarRecovery, DefUseCountedCorrectly) {
    BcMethod m = makeMethod("DefUseTest");
    m.locals.push_back(BcLocalVar{0, "x", types::Int()});
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDC_I4_1));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_STLOC_0));  // def
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDLOC_0));  // use
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_POP));

    CilStackSimulator sim;
    sim.simulate(m.cfg, m);
    CilVarRecovery recovery;
    auto result = recovery.recover(m.cfg, m, sim);
    ASSERT_EQ(1u, result.locals.size());
    EXPECT_EQ(1u, result.locals[0].defCount);
    EXPECT_EQ(1u, result.locals[0].useCount);
    // Single def + single use in same block → inlineable
    EXPECT_TRUE(result.locals[0].isInlineable);
}

// ─── Pattern detection tests ──────────────────────────────────────────────────

TEST(CilPatternDetector, DetectLinqChain) {
    CilRecoveredMethod method;
    method.method = nullptr;

    // Simulate a LINQ call: ExprCall to "Where" method
    ExprCall call;
    call.className  = "System.Linq.Enumerable";
    call.methodName = "Where";
    call.retType    = types::ClrObject();
    auto expr = std::make_shared<CilExpr>(std::move(call), types::ClrObject());

    CilStmt stmt;
    stmt.kind = StmtKind::ExprStmt;
    stmt.expr = expr;
    method.body.push_back(std::move(stmt));

    BcModule module("test", SourceLang::CSharp);

    CilPatternDetector detector;
    detector.detectLinqChains(method);
    EXPECT_TRUE(method.hasLinq);
}

TEST(CilPatternDetector, DetectPropertyGetter) {
    CilRecoveredMethod method;
    method.method = nullptr;

    ExprCall call;
    call.className  = "MyClass";
    call.methodName = "get_Name";
    call.retType    = types::ClrString();
    auto expr = std::make_shared<CilExpr>(std::move(call), types::ClrString());

    CilStmt stmt;
    stmt.kind = StmtKind::ExprStmt;
    stmt.expr = expr;
    method.body.push_back(std::move(stmt));

    BcModule module("test", SourceLang::CSharp);

    CilPatternDetector detector;
    detector.detectPropertyAccess(method);
    // The stmt.expr should be rewritten to ExprField
    ASSERT_NE(nullptr, method.body[0].expr);
    EXPECT_TRUE(std::holds_alternative<ExprField>(method.body[0].expr->data));
    auto& ef = std::get<ExprField>(method.body[0].expr->data);
    EXPECT_EQ("Name", ef.fieldName);
}

TEST(CilPatternDetector, DetectPropertySetter) {
    CilRecoveredMethod method;
    method.method = nullptr;

    ExprCall call;
    call.className  = "MyClass";
    call.methodName = "set_Value";
    call.retType    = types::Int();
    call.args.push_back(makeExprConst(42, types::Int()));
    auto expr = std::make_shared<CilExpr>(std::move(call), types::Int());

    CilStmt stmt;
    stmt.kind = StmtKind::ExprStmt;
    stmt.expr = expr;
    method.body.push_back(std::move(stmt));

    BcModule module("test", SourceLang::CSharp);

    CilPatternDetector detector;
    detector.detectPropertyAccess(method);
    EXPECT_EQ(StmtKind::Assign, method.body[0].kind);
    ASSERT_NE(nullptr, method.body[0].target);
    EXPECT_TRUE(std::holds_alternative<ExprField>(method.body[0].target->data));
    auto& ef = std::get<ExprField>(method.body[0].target->data);
    EXPECT_EQ("Value", ef.fieldName);
}

TEST(CilPatternDetector, DetectUnsafeLocalloc) {
    CilRecoveredMethod method;
    method.method = nullptr;
    method.hasUnsafe = false;

    ExprLocAlloc la{makeExprConst(10, types::Int()), types::Byte()};
    auto expr = std::make_shared<CilExpr>(std::move(la), types::Long());
    CilStmt stmt;
    stmt.kind = StmtKind::Assign;
    stmt.expr = expr;
    method.body.push_back(std::move(stmt));

    BcModule module("test", SourceLang::CSharp);
    CilPatternDetector detector;
    detector.detectUnsafePatterns(method);
    EXPECT_TRUE(method.hasUnsafe);
}

TEST(CilPatternDetector, NoFalsePositiveLinq) {
    CilRecoveredMethod method;
    method.method = nullptr;

    ExprCall call;
    call.className  = "MyClass";
    call.methodName = "DoSomething";
    call.retType    = types::Void();
    auto expr = std::make_shared<CilExpr>(std::move(call), types::Void());
    CilStmt stmt;
    stmt.kind = StmtKind::ExprStmt;
    stmt.expr = expr;
    method.body.push_back(std::move(stmt));

    BcModule module("test", SourceLang::CSharp);
    CilPatternDetector detector;
    detector.detectLinqChains(method);
    EXPECT_FALSE(method.hasLinq);
}

// ─── CilExpr factory tests ────────────────────────────────────────────────────

TEST(CilExpr, MakeConstInt) {
    auto e = makeExprConst(99, types::Int());
    ASSERT_NE(nullptr, e);
    EXPECT_TRUE(e->isConst());
    EXPECT_EQ(99, e->asConst().intVal);
    EXPECT_EQ(types::Int(), e->type);
}

TEST(CilExpr, MakeNull) {
    auto e = makeExprNull();
    ASSERT_NE(nullptr, e);
    EXPECT_TRUE(e->isNull());
}

TEST(CilExpr, MakeLocal) {
    auto e = makeExprLocal(3, "counter", types::Long());
    ASSERT_NE(nullptr, e);
    EXPECT_TRUE(e->isLocal());
    EXPECT_EQ(3u, e->asLocal().idx);
    EXPECT_EQ("counter", e->asLocal().name);
    EXPECT_EQ(types::Long(), e->type);
}

TEST(CilExpr, MakeArg) {
    auto e = makeExprArg(1, "value", types::Double());
    ASSERT_NE(nullptr, e);
    EXPECT_TRUE(e->isArg());
    EXPECT_EQ(1u, e->asArg().idx);
    EXPECT_EQ("value", e->asArg().name);
}

TEST(CilExpr, IsBinOp) {
    auto lhs = makeExprConst(1, types::Int());
    auto rhs = makeExprConst(2, types::Int());
    ExprBinOp b{BinOpKind::Add, lhs, rhs, types::Int()};
    auto e = std::make_shared<CilExpr>(std::move(b), types::Int());
    EXPECT_TRUE(e->isBinOp());
    EXPECT_EQ(BinOpKind::Add, e->asBinOp().op);
}

// ─── Full pipeline integration test ──────────────────────────────────────────

TEST(CilReconstructor, SimpleAddMethodReconstructsCleanly) {
    BcModule module("TestModule", SourceLang::CSharp);
    BcClass& cls = module.addClass(BcClass{});
    cls.name   = "Calculator";
    cls.fqName = "Calculator";

    BcMethod& m = cls.methods.emplace_back();
    m.name = "Add";
    m.descriptor.returnType = std::make_shared<BcType>(types::Int());
    m.descriptor.params.push_back(std::make_shared<BcType>(types::Int()));
    m.descriptor.params.push_back(std::make_shared<BcType>(types::Int()));
    m.paramNames = {"a", "b"};

    // Build CFG: ldarg.0 + ldarg.1 + add + ret
    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDARG_0));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LDARG_1));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_ADD));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_RET));

    CilReconstructor rec;
    auto result = rec.reconstruct(m, module);

    EXPECT_TRUE(result.success);
    EXPECT_GT(result.stmtCount + result.blockCount, 0u);
}

TEST(CilReconstructor, NativeMethodSkipped) {
    BcModule module("TestModule", SourceLang::CSharp);
    BcClass& cls = module.addClass(BcClass{});
    cls.name = "Foo"; cls.fqName = "Foo";

    BcMethod& m = cls.methods.emplace_back();
    m.name       = "NativeMethod";
    m.isNative   = true;
    m.isAbstract = true;
    // No CFG

    CilReconstructor rec;
    auto result = rec.reconstruct(m, module);
    // Should succeed (just return empty body)
    EXPECT_TRUE(result.success);
}

TEST(CilReconstructor, ReconstructAllFindsAllMethods) {
    BcModule module("M", SourceLang::CSharp);
    BcClass& cls = module.addClass(BcClass{});
    cls.name = "C"; cls.fqName = "C";

    for (int i = 0; i < 3; ++i) {
        BcMethod& m = cls.methods.emplace_back();
        m.name = "Method" + std::to_string(i);
        m.descriptor.returnType = std::make_shared<BcType>(types::Void());
        auto& b0 = m.cfg.addBlock();
        b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));
        b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_RET));
    }

    CilReconstructor rec;
    auto results = rec.reconstructAll(module);
    EXPECT_EQ(3u, results.size());
    for (auto& [k, v] : results) {
        EXPECT_TRUE(v.success) << "Failed for: " << k;
    }
}

TEST(CilReconstructor, EHHandlerBlockGetsCatchType) {
    BcModule module("EH", SourceLang::CSharp);
    BcClass& cls = module.addClass(BcClass{});
    cls.name = "EHTest"; cls.fqName = "EHTest";

    BcMethod& m = cls.methods.emplace_back();
    m.name = "TryCatch";
    m.descriptor.returnType = std::make_shared<BcType>(types::Void());

    auto& b0 = m.cfg.addBlock();
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_NOP));
    b0.instrs.push_back(makeInsn(BcOpcode::DOTNET_LEAVE));

    auto& b1 = m.cfg.addBlock();  // handler
    b1.isExceptionHandler = true;
    b1.instrs.push_back(makeInsn(BcOpcode::DOTNET_POP));
    b1.instrs.push_back(makeInsn(BcOpcode::DOTNET_LEAVE));

    BcExceptionHandler eh;
    eh.startOffset  = 0;
    eh.endOffset    = 2;
    eh.handlerBlock = 1;
    eh.catchType    = types::ClrObject();
    m.cfg.addExceptionHandler(std::move(eh));

    CilReconstructor rec;
    auto result = rec.reconstruct(m, module);
    EXPECT_TRUE(result.success);
}
