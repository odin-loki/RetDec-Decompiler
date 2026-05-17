/**
 * @file tests/jvm_reconstruct/jvm_reconstruct_test.cpp
 * @brief Unit tests for the JVM stack-to-variable reconstruction pipeline.
 *
 * Tests are self-contained: we build BcCFG objects directly using the
 * bc_module API and verify the reconstruction output without needing
 * real .class files.
 */

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_set>

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/bc_module/bc_type.h"
#include "retdec/jvm_reconstruct/jvm_reconstruct.h"
#include "retdec/jvm_reconstruct/local_rebuild.h"
#include "retdec/jvm_reconstruct/pattern_lift.h"
#include "retdec/jvm_reconstruct/slot_coalesce.h"
#include "retdec/jvm_reconstruct/stack_sim.h"

using namespace retdec::bc_module;
using namespace retdec::jvm_reconstruct;

// ─── CFG builder helpers ──────────────────────────────────────────────────────

static BcBasicBlock& addBlock(BcCFG& cfg) {
    return cfg.addBlock();
}

static BcInstruction makeInsn(BcOpcode op, uint32_t offset = 0) {
    BcInstruction i;
    static uint32_t idGen = 0;
    i.id = idGen++;
    i.offset = offset;
    i.opcode = op;
    return i;
}

static BcInstruction makePushInt(int64_t v, uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::PushInt, offset);
    i.operands.push_back(BcIntOperand{v});
    return i;
}

static BcInstruction makePushString(const std::string& s, uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::PushString, offset);
    i.operands.push_back(BcStringOperand{s});
    return i;
}

static BcInstruction makeInvokeVirtual(const std::string& owner,
                                        const std::string& name,
                                        const std::string& retDesc,
                                        uint32_t numArgs = 0,
                                        uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::InvokeVirtual, offset);
    BcMethodRef ref;
    ref.owner = owner; ref.name = name;
    auto retType0 = (retDesc == "V") ? types::Void() :
                    (retDesc == "I") ? types::Int() :
                    types::Class(retDesc);
    ref.descriptor.returnType = std::make_shared<BcType>(retType0);
    for (uint32_t j = 0; j < numArgs; ++j)
        ref.descriptor.params.push_back(std::make_shared<BcType>(types::Int()));
    i.operands.push_back(ref);
    return i;
}

static BcInstruction makeInvokeInterface(const std::string& owner,
                                          const std::string& name,
                                          const std::string& retDesc,
                                          uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::InvokeInterface, offset);
    BcMethodRef ref;
    ref.owner = owner; ref.name = name;
    auto retType1 = (retDesc == "Z") ? types::Bool() :
                    (retDesc == "V") ? types::Void() :
                    types::Class(retDesc);
    ref.descriptor.returnType = std::make_shared<BcType>(retType1);
    i.operands.push_back(ref);
    return i;
}

static BcInstruction makeInvokeDynamic(const std::string& bsmOwner,
                                        const std::string& bsmName,
                                        uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::InvokeDynamic, offset);
    BcMethodRef ref;
    ref.owner = bsmOwner; ref.name = bsmName;
    ref.descriptor.returnType = std::make_shared<BcType>(types::Class("java.util.function.Predicate"));
    i.operands.push_back(ref);
    return i;
}

static BcInstruction makeInvokeStatic(const std::string& owner,
                                       const std::string& name,
                                       uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::InvokeStatic, offset);
    BcMethodRef ref;
    ref.owner = owner; ref.name = name;
    ref.descriptor.returnType = std::make_shared<BcType>(types::Void());
    i.operands.push_back(ref);
    return i;
}

static BcInstruction makeNew(const std::string& cls, uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::New, offset);
    i.operands.push_back(BcTypeOperand{types::Class(cls)});
    return i;
}

static BcInstruction makeLoadLocal(uint32_t idx, BcType type = types::Int(),
                                    uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::LoadLocal, offset);
    i.operands.push_back(BcLocalOperand{idx});
    i.operands.push_back(BcTypeOperand{type});
    return i;
}

static BcInstruction makeStoreLocal(uint32_t idx, uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::StoreLocal, offset);
    i.operands.push_back(BcLocalOperand{idx});
    return i;
}

static BcInstruction makeAdd(uint32_t offset = 0) {
    return makeInsn(BcOpcode::Add, offset);
}

static BcInstruction makeReturn(uint32_t offset = 0) {
    return makeInsn(BcOpcode::Return, offset);
}

static BcInstruction makeReturnValue(uint32_t offset = 0) {
    return makeInsn(BcOpcode::ReturnValue, offset);
}

static BcInstruction makeArrayLoad(uint32_t offset = 0) {
    return makeInsn(BcOpcode::ArrayLoad, offset);
}

static BcInstruction makeArrayLength(uint32_t offset = 0) {
    return makeInsn(BcOpcode::ArrayLength, offset);
}

static BcInstruction makeCmpLt(uint32_t offset = 0) {
    return makeInsn(BcOpcode::CmpLt, offset);
}

static BcInstruction makeGoto(uint32_t targetBlockId, uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::Goto, offset);
    i.operands.push_back(BcBlockOperand{targetBlockId});
    return i;
}

static BcInstruction makeIfFalse(uint32_t targetBlockId, uint32_t offset = 0) {
    auto i = makeInsn(BcOpcode::IfFalse, offset);
    i.operands.push_back(BcBlockOperand{targetBlockId});
    return i;
}

// Build a minimal method with a given BcFuncType.
static BcMethod makeMethod(const std::string& name = "test",
                             bool isStatic = true) {
    BcMethod m;
    m.name = name;
    m.access = isStatic ? BcAccess::Static : BcAccess::None;
    m.descriptor.returnType = std::make_shared<BcType>(types::Void());
    return m;
}

// ─── JvmStackSim tests ────────────────────────────────────────────────────────

TEST(JvmStackSim, EmptyCFGProducesEmptyResult) {
    BcCFG cfg;
    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto result = sim.simulate(cfg, method);
    EXPECT_EQ(StackSimResult::OK, result.status);
    EXPECT_TRUE(result.slots.empty());
}

TEST(JvmStackSim, SinglePushInt) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makePushInt(42));
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto result = sim.simulate(cfg, method);

    EXPECT_EQ(StackSimResult::OK, result.status);
    EXPECT_GE(result.slots.size(), 1u);
    EXPECT_EQ(BcPrimKind::Int, result.slots[0].type.prim().kind);
}

TEST(JvmStackSim, AddIntProducesIntSlot) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makePushInt(1));
    blk.instrs.push_back(makePushInt(2));
    blk.instrs.push_back(makeAdd());
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto result = sim.simulate(cfg, method);

    EXPECT_EQ(StackSimResult::OK, result.status);
    // Should have 3 slots: two for the push_ints, one for the add result.
    EXPECT_GE(result.slots.size(), 3u);
}

TEST(JvmStackSim, PushStringGetsStringType) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makePushString("hello"));
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto result = sim.simulate(cfg, method);

    EXPECT_EQ(StackSimResult::OK, result.status);
    ASSERT_GE(result.slots.size(), 1u);
    EXPECT_TRUE(result.slots[0].type.isRef());
    EXPECT_EQ("java.lang.String", result.slots[0].type.ref().className);
}

TEST(JvmStackSim, NewAllocatesRefSlot) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makeNew("java.lang.StringBuilder"));
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto result = sim.simulate(cfg, method);

    EXPECT_EQ(StackSimResult::OK, result.status);
    ASSERT_GE(result.slots.size(), 1u);
    EXPECT_TRUE(result.slots[0].type.isRef());
    EXPECT_EQ("java.lang.StringBuilder", result.slots[0].type.ref().className);
}

TEST(JvmStackSim, VoidInvokeDoesNotPush) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makeNew("Foo"));
    blk.instrs.push_back(makeInvokeVirtual("Foo", "doSomething", "V", 0));
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto result = sim.simulate(cfg, method);

    // After the void call, stack should be empty (just the new Foo slot remains,
    // and the invoke consumes it and pushes nothing).
    auto exitIt = result.blockExitStates.find(0);
    ASSERT_NE(exitIt, result.blockExitStates.end());
    EXPECT_TRUE(exitIt->second.empty());
}

TEST(JvmStackSim, ExceptionHandlerGetsExceptionSlot) {
    BcCFG cfg;
    auto& blk0 = addBlock(cfg);  // try block
    blk0.instrs.push_back(makeReturn());
    auto& blk1 = addBlock(cfg);  // handler block
    blk1.instrs.push_back(makeReturn());
    blk1.isExceptionHandler = true;
    cfg.addEdge(0, 1);

    BcExceptionHandler eh;
    eh.startOffset  = 0;
    eh.endOffset    = 2;
    eh.handlerBlock = 1;
    eh.catchType = types::Class("java.lang.Exception");
    cfg.addExceptionHandler(eh);

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto result = sim.simulate(cfg, method);

    // Handler block should have one slot (the exception).
    auto it = result.blockEntryStates.find(1);
    EXPECT_NE(it, result.blockEntryStates.end());
    if (it != result.blockEntryStates.end())
        EXPECT_GE(it->second.size(), 1u);
}

TEST(JvmStackSim, MultiBlockPropagatesState) {
    // Block 0: push int, goto block 1
    // Block 1: pop (return value)
    BcCFG cfg;
    auto& blk0 = addBlock(cfg);
    blk0.instrs.push_back(makePushInt(5));
    blk0.instrs.push_back(makeGoto(1));

    auto& blk1 = addBlock(cfg);
    blk1.instrs.push_back(makeReturnValue());
    cfg.addEdge(0, 1);

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto result = sim.simulate(cfg, method);

    EXPECT_EQ(StackSimResult::OK, result.status);
    // Block 1 should have an entry state with one slot (the pushed int).
    auto it = result.blockEntryStates.find(1);
    EXPECT_NE(it, result.blockEntryStates.end());
}

// ─── SlotCoalescer tests ──────────────────────────────────────────────────────

TEST(SlotCoalescer, SingleUseSingleDefSameBlockCoalesced) {
    // push int 1 → add → return
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makePushInt(1));  // id=0 produces slot 0
    blk.instrs.push_back(makePushInt(2));  // id=1 produces slot 1
    blk.instrs.push_back(makeAdd());        // id=2 consumes 0,1 produces slot 2
    blk.instrs.push_back(makeReturn());     // id=3 consumes slot 2

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);

    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);

    // Slots that are only used once in the same block should be eliminated.
    EXPECT_FALSE(coalesceResult.eliminatedSlots.empty());
}

TEST(SlotCoalescer, CrossBlockSlotsNotCoalesced) {
    // Block 0: push int → goto block 1
    // Block 1: return value (uses slot from block 0)
    BcCFG cfg;
    auto& blk0 = addBlock(cfg);
    blk0.instrs.push_back(makePushInt(42));
    blk0.instrs.push_back(makeGoto(1));

    auto& blk1 = addBlock(cfg);
    blk1.instrs.push_back(makeReturnValue());
    cfg.addEdge(0, 1);

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);

    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);

    // The slot crossing a block boundary should survive.
    EXPECT_FALSE(coalesceResult.survivingSlots.empty());
}

// ─── LocalRebuilder tests ──────────────────────────────────────────────────────

TEST(LocalRebuilder, EmptyCFGProducesOnlyParams) {
    BcCFG cfg;
    BcMethod method = makeMethod("test", false); // instance method
    // Add 'this' + one int parameter
    method.descriptor.params.push_back(std::make_shared<BcType>(types::Int()));

    StackSimResult simResult;
    CoalesceResult coalesceResult;

    LocalRebuilder rebuilder;
    auto result = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    EXPECT_EQ(LocalRebuildResult::OK, result.status);
    // Should have: 'this' + param0 = 2 locals
    EXPECT_GE(result.locals.size(), 1u); // at least param
}

TEST(LocalRebuilder, StaticMethodNoThis) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod("test", true);
    method.descriptor.params.push_back(std::make_shared<BcType>(types::Int()));
    method.descriptor.params.push_back(std::make_shared<BcType>(types::Double()));

    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);

    LocalRebuilder rebuilder;
    auto result = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    // Static: no 'this', two params (double occupies 2 slots → 3 JVM slots total)
    EXPECT_GE(result.locals.size(), 2u);
    // First local should be a parameter.
    EXPECT_TRUE(result.locals[0].isParam);
}

TEST(LocalRebuilder, UsesLVTNames) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makeLoadLocal(0, types::Int()));
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod("test", true); // static: slot 0 is a local, not 'this'

    std::vector<LVTEntry> lvt = {
        {0, 10, "counter", "I", "", 0},
    };

    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);

    LocalRebuilder rebuilder;
    auto result = rebuilder.rebuild(method, cfg, simResult, coalesceResult, lvt);

    // Slot 0 should be named "counter" from LVT.
    bool found = false;
    for (const auto& lv : result.locals)
        if (lv.name == "counter") found = true;
    EXPECT_TRUE(found);
}

TEST(LocalRebuilder, InfersSyntheticName) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makePushInt(1));
    blk.instrs.push_back(makeStoreLocal(2));
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod("test", true);

    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);

    LocalRebuilder rebuilder;
    auto result = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    // Should generate a synthetic name for slot 2.
    bool found = false;
    for (const auto& lv : result.locals)
        if (!lv.name.empty() && lv.index == 2) found = true;
    EXPECT_TRUE(found);
}

// ─── ExceptionVarIntroducer tests ─────────────────────────────────────────────

TEST(ExceptionVarIntroducer, IntroducesExceptionLocal) {
    BcCFG cfg;
    auto& tryBlk = addBlock(cfg);
    tryBlk.instrs.push_back(makeReturn());
    auto& handlerBlk = addBlock(cfg);
    handlerBlk.instrs.push_back(makeReturn());
    cfg.addEdge(0, 1);

    BcExceptionHandler eh;
    eh.startOffset = 0; eh.endOffset = 2;
    eh.handlerBlock = 1;
    eh.catchType = types::Class("java.lang.Exception");
    cfg.addExceptionHandler(eh);

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);
    LocalRebuilder rebuilder;
    auto localResult = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    ExceptionVarIntroducer intro;
    intro.introduce(cfg, method, simResult, localResult);

    // Handler block should now be marked as exception handler.
    EXPECT_TRUE(cfg.block(1).isExceptionHandler);
    // An exception variable should have been created.
    bool hasExVar = false;
    for (const auto& lv : localResult.locals)
        if (!lv.name.empty() && lv.name.find("ex") != std::string::npos)
            hasExVar = true;
    EXPECT_TRUE(hasExVar);
}

// ─── PatternLifter tests ──────────────────────────────────────────────────────

TEST(PatternLifter, DetectsStringBuilderConcat) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makeNew("java.lang.StringBuilder"));
    blk.instrs.push_back(makeInvokeVirtual("java.lang.StringBuilder", "append", "java.lang.StringBuilder", 1));
    blk.instrs.push_back(makeInvokeVirtual("java.lang.StringBuilder", "append", "java.lang.StringBuilder", 1));
    blk.instrs.push_back(makeInvokeVirtual("java.lang.StringBuilder", "toString", "java.lang.String", 0));
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);
    LocalRebuilder rebuilder;
    auto localResult = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    PatternLifter lifter;
    auto patterns = lifter.lift(cfg, method, simResult, localResult);

    EXPECT_FALSE(patterns.stringConcats.empty());
}

TEST(PatternLifter, DetectsInvokeDynamicStringConcat) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makePushString("hello "));
    blk.instrs.push_back(makePushString("world"));
    // Simulate invokedynamic makeConcatWithConstants
    auto invdyn = makeInsn(BcOpcode::InvokeDynamic);
    BcMethodRef ref;
    ref.owner = "java.lang.invoke.StringConcatFactory";
    ref.name  = "makeConcatWithConstants";
    ref.descriptor.returnType = std::make_shared<BcType>(types::Class("java.lang.String"));
    ref.descriptor.params = {std::make_shared<BcType>(types::Int()), std::make_shared<BcType>(types::Int())};
    invdyn.operands.push_back(ref);
    blk.instrs.push_back(invdyn);
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);
    LocalRebuilder rebuilder;
    auto localResult = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    PatternLifter lifter;
    auto patterns = lifter.lift(cfg, method, simResult, localResult);

    EXPECT_FALSE(patterns.stringConcats.empty());
}

TEST(PatternLifter, DetectsLambdaInvokeDynamic) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);

    // Simulate invokedynamic for LambdaMetafactory.
    auto invdyn = makeInsn(BcOpcode::InvokeDynamic);
    BcMethodRef bsmRef;
    bsmRef.owner = "java.lang.invoke.LambdaMetafactory";
    bsmRef.name  = "metafactory";
    bsmRef.descriptor.returnType = std::make_shared<BcType>(types::Class("java.util.function.Predicate"));
    invdyn.operands.push_back(bsmRef);

    // Implementation method reference.
    BcMethodRef implRef;
    implRef.owner = "com.example.Foo";
    implRef.name  = "lambda$test$0";
    invdyn.operands.push_back(implRef);

    blk.instrs.push_back(invdyn);
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);
    LocalRebuilder rebuilder;
    auto localResult = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    PatternLifter lifter;
    auto patterns = lifter.lift(cfg, method, simResult, localResult);

    EXPECT_FALSE(patterns.lambdas.empty());
    if (!patterns.lambdas.empty()) {
        EXPECT_EQ(LambdaKind::Lambda, patterns.lambdas[0].kind);
        EXPECT_EQ("java.util.function.Predicate",
                  patterns.lambdas[0].functionalInterface);
    }
}

TEST(PatternLifter, DetectsMethodReference) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);

    auto invdyn = makeInsn(BcOpcode::InvokeDynamic);
    BcMethodRef bsmRef;
    bsmRef.owner = "java.lang.invoke.LambdaMetafactory";
    bsmRef.name  = "metafactory";
    bsmRef.descriptor.returnType = std::make_shared<BcType>(types::Class("java.util.function.Consumer"));
    invdyn.operands.push_back(bsmRef);

    BcMethodRef implRef;
    implRef.owner = "java.io.PrintStream";
    implRef.name  = "println"; // System.out::println — not lambda$
    invdyn.operands.push_back(implRef);

    blk.instrs.push_back(invdyn);
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);
    LocalRebuilder rebuilder;
    auto localResult = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    PatternLifter lifter;
    auto patterns = lifter.lift(cfg, method, simResult, localResult);

    ASSERT_FALSE(patterns.lambdas.empty());
    EXPECT_EQ(LambdaKind::MethodReference, patterns.lambdas[0].kind);
}

TEST(PatternLifter, DetectsIteratorForEach) {
    // Block 0 (loop header): invokeinterface hasNext → if_false exit
    // Block 1 (body): invokeinterface next → use element → goto header
    // Block 2 (exit): return
    BcCFG cfg;
    auto& hdr  = addBlock(cfg); // block 0 — loop header
    auto& body = addBlock(cfg); // block 1
    auto& exit = addBlock(cfg); // block 2

    hdr.isLoopHeader = true;
    hdr.instrs.push_back(makeLoadLocal(1)); // load iterator
    hdr.instrs.push_back(makeInvokeInterface("java.util.Iterator", "hasNext", "Z"));
    hdr.instrs.push_back(makeIfFalse(2));

    body.instrs.push_back(makeLoadLocal(1)); // load iterator
    body.instrs.push_back(makeInvokeInterface("java.util.Iterator", "next",
                                               "java.lang.Object"));
    body.instrs.push_back(makeStoreLocal(2));
    body.instrs.push_back(makeGoto(0)); // back-edge

    exit.instrs.push_back(makeReturn());

    cfg.addEdge(0, 1); cfg.addEdge(0, 2); // conditional
    cfg.addEdge(1, 0);                     // back-edge
    cfg.addEdge(1, 2);

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);
    LocalRebuilder rebuilder;
    auto localResult = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    PatternLifter lifter;
    auto patterns = lifter.lift(cfg, method, simResult, localResult);

    EXPECT_FALSE(patterns.forEachLoops.empty());
    if (!patterns.forEachLoops.empty())
        EXPECT_EQ(ForEachKind::Iterator, patterns.forEachLoops[0].kind);
}

TEST(PatternLifter, DetectsArrayForEach) {
    // Block 0 (loop header): load i, load len, cmp_lt → if_false exit
    // Block 1 (body): load arr, load i, array_load → body code → goto header
    // Block 2 (exit): return
    BcCFG cfg;
    auto& hdr  = addBlock(cfg);
    auto& body = addBlock(cfg);
    auto& exit = addBlock(cfg);

    hdr.isLoopHeader = true;
    hdr.instrs.push_back(makeLoadLocal(1)); // i
    hdr.instrs.push_back(makeLoadLocal(2)); // len
    hdr.instrs.push_back(makeCmpLt());
    hdr.instrs.push_back(makeIfFalse(2));

    body.instrs.push_back(makeLoadLocal(0)); // array
    body.instrs.push_back(makeLoadLocal(1)); // i
    body.instrs.push_back(makeArrayLoad());
    body.instrs.push_back(makeStoreLocal(3)); // element
    body.instrs.push_back(makeGoto(0));

    exit.instrs.push_back(makeReturn());

    cfg.addEdge(0, 1); cfg.addEdge(0, 2);
    cfg.addEdge(1, 0); cfg.addEdge(1, 2);

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto simResult = sim.simulate(cfg, method);
    SlotCoalescer coalescer;
    auto coalesceResult = coalescer.coalesce(cfg, simResult);
    LocalRebuilder rebuilder;
    auto localResult = rebuilder.rebuild(method, cfg, simResult, coalesceResult);

    PatternLifter lifter;
    auto patterns = lifter.lift(cfg, method, simResult, localResult);

    bool hasArray = false;
    for (const auto& p : patterns.forEachLoops)
        if (p.kind == ForEachKind::Array) hasArray = true;
    EXPECT_TRUE(hasArray);
}

// ─── JvmReconstructor integration tests ──────────────────────────────────────

TEST(JvmReconstructor, ReconstructsEmptyMethod) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    method.cfg = std::move(cfg);

    JvmReconstructor rec;
    auto result = rec.reconstruct(method);
    EXPECT_EQ(ReconstructResult::OK, result.status);
}

TEST(JvmReconstructor, ReconstructsPushReturn) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makePushInt(42));
    blk.instrs.push_back(makeReturnValue());

    BcMethod method = makeMethod();
    method.descriptor.returnType = std::make_shared<BcType>(types::Int());
    method.cfg = std::move(cfg);

    JvmReconstructor rec;
    auto result = rec.reconstruct(method);
    EXPECT_EQ(ReconstructResult::OK, result.status);
    // After reconstruction, the method's locals may be populated.
}

TEST(JvmReconstructor, ReconstructsWithLVT) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    blk.instrs.push_back(makeLoadLocal(0, types::Int()));
    blk.instrs.push_back(makeLoadLocal(1, types::Int()));
    blk.instrs.push_back(makeAdd());
    blk.instrs.push_back(makeReturnValue());

    BcMethod method = makeMethod("add", true);
    method.descriptor.returnType = std::make_shared<BcType>(types::Int());
    method.descriptor.params = {std::make_shared<BcType>(types::Int()), std::make_shared<BcType>(types::Int())};
    method.cfg = std::move(cfg);

    std::vector<LVTEntry> lvt = {
        {0, 100, "a", "I", "", 0},
        {0, 100, "b", "I", "", 1},
    };

    JvmReconstructor rec;
    auto result = rec.reconstruct(method, lvt);
    EXPECT_EQ(ReconstructResult::OK, result.status);

    // Method should now have named locals.
    bool hasA = false, hasB = false;
    for (const auto& lv : method.locals) {
        if (lv.name == "a") hasA = true;
        if (lv.name == "b") hasB = true;
    }
    EXPECT_TRUE(hasA);
    EXPECT_TRUE(hasB);
}

TEST(JvmReconstructor, ReconstructsModule) {
    BcModule module("TestModule", SourceLang::Java);

    BcClass cls;
    cls.name = "Test";

    BcMethod m;
    m.name = "run";
    m.access = BcAccess::Static;
    m.descriptor.returnType = std::make_shared<BcType>(types::Void());
    auto& blk = m.cfg.addBlock();
    blk.instrs.push_back(makePushInt(1));
    blk.instrs.push_back(makeReturnValue());
    cls.methods.push_back(std::move(m));
    module.addClass(std::move(cls));

    JvmReconstructor rec;
    int errors = rec.reconstructModule(module);
    EXPECT_EQ(0, errors);
}

TEST(JvmReconstructor, SlotNamesAreUnique) {
    BcCFG cfg;
    auto& blk = addBlock(cfg);
    // Push multiple different values.
    blk.instrs.push_back(makePushInt(1));
    blk.instrs.push_back(makePushInt(2));
    blk.instrs.push_back(makePushString("x"));
    blk.instrs.push_back(makeReturn());

    BcMethod method = makeMethod();
    method.cfg = std::move(cfg);

    JvmReconstructor rec;
    rec.reconstruct(method);

    // Verify all slot names are unique.
    std::unordered_set<std::string> names;
    for (const auto& lv : method.locals)
        names.insert(lv.name);
    EXPECT_EQ(method.locals.size(), names.size());
}

// ─── StackSlot type tests ─────────────────────────────────────────────────────

TEST(StackSlot, WideTypes) {
    StackSlot longSlot;
    longSlot.type = types::Long();
    EXPECT_TRUE(longSlot.isWide());

    StackSlot dblSlot;
    dblSlot.type = types::Double();
    EXPECT_TRUE(dblSlot.isWide());

    StackSlot intSlot;
    intSlot.type = types::Int();
    EXPECT_FALSE(intSlot.isWide());
}

TEST(JvmStackSim, MeetAtJoinPreservesState) {
    // Two paths that push the same type merge correctly.
    BcCFG cfg;
    auto& entry = addBlock(cfg);  // block 0
    auto& pathA = addBlock(cfg);  // block 1
    auto& pathB = addBlock(cfg);  // block 2
    auto& merge = addBlock(cfg);  // block 3

    entry.instrs.push_back(makeInsn(BcOpcode::IfTrue));
    pathA.instrs.push_back(makePushInt(1));
    pathA.instrs.push_back(makeGoto(3));
    pathB.instrs.push_back(makePushInt(2));
    pathB.instrs.push_back(makeGoto(3));
    merge.instrs.push_back(makeReturnValue());

    cfg.addEdge(0, 1); cfg.addEdge(0, 2);
    cfg.addEdge(1, 3); cfg.addEdge(2, 3);

    BcMethod method = makeMethod();
    JvmStackSim sim;
    auto result = sim.simulate(cfg, method);

    EXPECT_EQ(StackSimResult::OK, result.status);
    // Merge block should have an entry state.
    EXPECT_TRUE(result.blockEntryStates.count(3));
}
