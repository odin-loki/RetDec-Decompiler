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
