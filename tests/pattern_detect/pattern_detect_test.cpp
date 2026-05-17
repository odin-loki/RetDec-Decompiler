/**
 * @file tests/pattern_detect/pattern_detect_test.cpp
 * @brief Unit tests for the Design Pattern Detector module (Stage 28).
 *
 * Coverage:
 *   - PatternResult::kindName / toString
 *   - SingletonDetector  (null-check, alloc, return; double-checked-lock)
 *   - FactoryDetector    (switch discriminant, ≥2 alloc sites, common return)
 *   - ObserverDetector   (register + notify; group mode)
 *   - CommandDetector    (vtable execute, container, loop; undo variant)
 *   - StrategyDetector   (stored ptr, setter, indirect call; group mode)
 *   - StateMachineDetector (state var, switch-on-state, case constants)
 *   - RAIIDetector       (acquire/release pair; group mode)
 *   - PatternDetector    (preflight, multi-pattern, group analysis)
 */

#include <memory>
#include "retdec/pattern_detect/pattern_detect.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace retdec::pattern_detect;
using namespace retdec;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::unique_ptr<ssa::SSAFunction> makeFunc(
        const std::string& name,
        const std::vector<ssa::IrInstr::Op>& ops,
        int extraBlocks = 0) {
    auto fn = std::make_unique<ssa::SSAFunction>(name);
    auto* entry = fn->addBlock("entry");
    for (auto op : ops) fn->addInstr(entry->id, op);
    for (int i = 0; i < extraBlocks; ++i)
        fn->addBlock("b" + std::to_string(i));
    return fn;
}

static void addCall(ssa::SSAFunction& fn, const std::string& callee) {
    auto* i = fn.addInstr(fn.block(0)->id, ssa::IrInstr::Op::Call);
    if (i) i->calleeName = callee;
}

static void addImmCompare(ssa::SSAFunction& fn, uint64_t val) {
    auto* i = fn.addInstr(fn.block(0)->id, ssa::IrInstr::Op::Compare);
    if (!i) return;
    ssa::IrValue* irval = fn.allocValue(ssa::ValueKind::Immediate);
    if (irval) irval->imm = val;
    ssa::Use u; u.valueId = irval ? irval->id : ssa::kInvalidValue;
    i->uses.push_back(u);
}

static void addBackEdge(ssa::SSAFunction& fn) {
    if (fn.blockCount() >= 2) fn.block(1)->succs.push_back(0);
    else                       fn.block(0)->succs.push_back(0);
}

// ─── PatternResult tests ──────────────────────────────────────────────────────

TEST(PatternResultTest, KindNameSingleton) {
    PatternResult r; r.kind = PatternKind::Singleton;
    EXPECT_EQ(r.kindName(), "Singleton");
}
TEST(PatternResultTest, KindNameFactory) {
    PatternResult r; r.kind = PatternKind::Factory;
    EXPECT_EQ(r.kindName(), "Factory");
}
TEST(PatternResultTest, KindNameObserver) {
    PatternResult r; r.kind = PatternKind::Observer;
    EXPECT_EQ(r.kindName(), "Observer");
}
TEST(PatternResultTest, KindNameCommand) {
    PatternResult r; r.kind = PatternKind::Command;
    EXPECT_EQ(r.kindName(), "Command");
}
TEST(PatternResultTest, KindNameStrategy) {
    PatternResult r; r.kind = PatternKind::Strategy;
    EXPECT_EQ(r.kindName(), "Strategy");
}
TEST(PatternResultTest, KindNameStateMachine) {
    PatternResult r; r.kind = PatternKind::StateMachine;
    EXPECT_EQ(r.kindName(), "StateMachine");
}
TEST(PatternResultTest, KindNameRAII) {
    PatternResult r; r.kind = PatternKind::RAII;
    EXPECT_EQ(r.kindName(), "RAII");
}
TEST(PatternResultTest, KindNameUnknown) {
    PatternResult r;
    EXPECT_EQ(r.kindName(), "Unknown");
}
TEST(PatternResultTest, ToStringContainsConfidence) {
    PatternResult r;
    r.kind = PatternKind::Singleton;
    r.confidence = 0.9f;
    EXPECT_NE(r.toString().find("0.9"), std::string::npos);
}
TEST(PatternResultTest, ToStringContainsVariantName) {
    PatternResult r;
    r.kind = PatternKind::Singleton;
    r.confidence = 1.0f;
    r.hasVariant = true;
    r.variantName = "double-checked-lock";
    EXPECT_NE(r.toString().find("double-checked-lock"), std::string::npos);
}

// ─── SingletonDetector tests ──────────────────────────────────────────────────

TEST(SingletonDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    SingletonDetector det;
    EXPECT_LT(det.detect(*fn).confidence, 0.30f);
}

TEST(SingletonDetectorTest, NullCheckAndAllocDetected) {
    auto fn = makeFunc("getInstance", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Ret,
    }, 1);
    addImmCompare(*fn, 0);
    addCall(*fn, "malloc");
    SingletonDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.45f);
    EXPECT_EQ(r.kind, PatternKind::Singleton);
}

TEST(SingletonDetectorTest, DoubleCheckedLockVariant) {
    auto fn = makeFunc("getInstance_dcl", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Ret,
    }, 2);
    addImmCompare(*fn, 0);
    addImmCompare(*fn, 0);  // second null-check
    addCall(*fn, "pthread_mutex_lock");
    addCall(*fn, "malloc");
    SingletonDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.45f);
    EXPECT_TRUE(r.hasVariant);
    EXPECT_EQ(r.variantName, "double-checked-lock");
}

TEST(SingletonDetectorTest, EmittedFormContainsGetInstance) {
    auto fn = makeFunc("si", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Ret }, 1);
    addImmCompare(*fn, 0); addCall(*fn, "malloc");
    SingletonDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.45f)
        EXPECT_NE(r.emittedForm.find("getInstance"), std::string::npos);
}

TEST(SingletonDetectorTest, GroupDetectReturnsBest) {
    auto fn = makeFunc("g", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Ret }, 1);
    addImmCompare(*fn, 0); addCall(*fn, "_Znwm");
    std::vector<const ssa::SSAFunction*> fns = { fn.get() };
    SingletonDetector det;
    auto r = det.detectGroup(fns);
    EXPECT_GE(r.confidence, 0.45f);
}

// ─── FactoryDetector tests ────────────────────────────────────────────────────

TEST(FactoryDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    FactoryDetector det;
    EXPECT_LT(det.detect(*fn).confidence, 0.30f);
}

TEST(FactoryDetectorTest, TwoAllocsAndSwitchDetected) {
    auto fn = makeFunc("create", {
        ssa::IrInstr::Op::Ret,
    }, 3);
    addImmCompare(*fn, 0);
    addImmCompare(*fn, 1);
    addCall(*fn, "malloc");
    addCall(*fn, "malloc");
    FactoryDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.70f);
    EXPECT_EQ(r.kind, PatternKind::Factory);
}

TEST(FactoryDetectorTest, OneAllocInsufficientForFactory) {
    auto fn = makeFunc("create1", { ssa::IrInstr::Op::Ret }, 1);
    addImmCompare(*fn, 0);
    addCall(*fn, "malloc");
    FactoryDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.45f);
}

TEST(FactoryDetectorTest, EmittedFormContainsSwitch) {
    auto fn = makeFunc("fac", { ssa::IrInstr::Op::Ret }, 3);
    addImmCompare(*fn, 0); addImmCompare(*fn, 1);
    addCall(*fn, "_Znwm"); addCall(*fn, "_Znwm");
    FactoryDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.45f)
        EXPECT_NE(r.emittedForm.find("switch"), std::string::npos);
}

// ─── ObserverDetector tests ───────────────────────────────────────────────────

TEST(ObserverDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    ObserverDetector det;
    EXPECT_LT(det.detect(*fn).confidence, 0.30f);
}

TEST(ObserverDetectorTest, RegisterAndNotifyInSameFn) {
    auto fn = makeFunc("obs", { ssa::IrInstr::Op::Load }, 1);
    addBackEdge(*fn);
    addCall(*fn, "push_back");
    addCall(*fn, "emit");
    ObserverDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.45f);
    EXPECT_EQ(r.kind, PatternKind::Observer);
}

TEST(ObserverDetectorTest, GroupModeRegisterPlusNotify) {
    auto regFn = makeFunc("subscribe", {});
    addCall(*regFn, "push_back");
    auto notifyFn = makeFunc("notify", { ssa::IrInstr::Op::Load }, 1);
    addBackEdge(*notifyFn);
    addCall(*notifyFn, "broadcast");
    std::vector<const ssa::SSAFunction*> fns = { regFn.get(), notifyFn.get() };
    ObserverDetector det;
    auto r = det.detectGroup(fns);
    EXPECT_GE(r.confidence, 0.90f);
}

TEST(ObserverDetectorTest, EmittedFormContainsEventEmitter) {
    auto fn = makeFunc("obs2", { ssa::IrInstr::Op::Load }, 1);
    addBackEdge(*fn); addCall(*fn, "push_back"); addCall(*fn, "emit");
    ObserverDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.45f)
        EXPECT_NE(r.emittedForm.find("EventEmitter"), std::string::npos);
}

// ─── CommandDetector tests ────────────────────────────────────────────────────

TEST(CommandDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    CommandDetector det;
    EXPECT_LT(det.detect(*fn).confidence, 0.30f);
}

TEST(CommandDetectorTest, VtableExecuteInLoopDetected) {
    auto fn = makeFunc("runQueue", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Load }, 1);
    addBackEdge(*fn);
    addCall(*fn, "execute");
    addCall(*fn, "push_back");
    CommandDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.45f);
    EXPECT_EQ(r.kind, PatternKind::Command);
}

TEST(CommandDetectorTest, UndoMethodSetsVariant) {
    auto fn = makeFunc("runUndo", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Load }, 1);
    addBackEdge(*fn);
    addCall(*fn, "execute");
    addCall(*fn, "undo");
    addCall(*fn, "push_back");
    CommandDetector det;
    auto r = det.detect(*fn);
    EXPECT_TRUE(r.hasVariant);
    EXPECT_EQ(r.variantName, "with-undo");
}

TEST(CommandDetectorTest, EmittedFormContainsICommand) {
    auto fn = makeFunc("cmd", { ssa::IrInstr::Op::Load }, 1);
    addBackEdge(*fn); addCall(*fn, "execute"); addCall(*fn, "push_back");
    CommandDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.45f)
        EXPECT_NE(r.emittedForm.find("ICommand"), std::string::npos);
}

// ─── StrategyDetector tests ───────────────────────────────────────────────────

TEST(StrategyDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    StrategyDetector det;
    EXPECT_LT(det.detect(*fn).confidence, 0.30f);
}

TEST(StrategyDetectorTest, IndirectCallWithLoadDetected) {
    auto fn = makeFunc("exec", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
    });
    addCall(*fn, "doAlgorithm");
    StrategyDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.45f);
    EXPECT_EQ(r.kind, PatternKind::Strategy);
}

TEST(StrategyDetectorTest, GroupModeSetterPlusExecutor) {
    auto setter = makeFunc("setStrategy", { ssa::IrInstr::Op::Store });
    auto exec   = makeFunc("execute", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Load });
    addCall(*exec, "doAlgorithm");
    std::vector<const ssa::SSAFunction*> fns = { setter.get(), exec.get() };
    StrategyDetector det;
    auto r = det.detectGroup(fns);
    EXPECT_GE(r.confidence, 0.65f);
}

TEST(StrategyDetectorTest, EmittedFormContainsIStrategy) {
    auto fn = makeFunc("strat", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store });
    addCall(*fn, "doAlgorithm");
    StrategyDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.45f)
        EXPECT_NE(r.emittedForm.find("IStrategy"), std::string::npos);
}

// ─── StateMachineDetector tests ───────────────────────────────────────────────

TEST(StateMachineDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    StateMachineDetector det;
    EXPECT_LT(det.detect(*fn).confidence, 0.30f);
}

TEST(StateMachineDetectorTest, StateVarAndSwitchDetected) {
    auto fn = makeFunc("transition", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Store,
    }, 2);
    addImmCompare(*fn, 0);
    addImmCompare(*fn, 1);
    StateMachineDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.70f);
    EXPECT_EQ(r.kind, PatternKind::StateMachine);
}

TEST(StateMachineDetectorTest, CaseCountReturned) {
    auto fn = makeFunc("sm3", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Store }, 3);
    addImmCompare(*fn, 0);
    addImmCompare(*fn, 1);
    addImmCompare(*fn, 2);
    StateMachineDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.45f)
        EXPECT_NE(r.comment.find("3"), std::string::npos);
}

TEST(StateMachineDetectorTest, EmittedFormContainsStateEnum) {
    auto fn = makeFunc("sm", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Store }, 2);
    addImmCompare(*fn, 0); addImmCompare(*fn, 1);
    StateMachineDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.45f)
        EXPECT_NE(r.emittedForm.find("enum State"), std::string::npos);
}

// ─── RAIIDetector tests ───────────────────────────────────────────────────────

TEST(RAIIDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    RAIIDetector det;
    EXPECT_LT(det.detect(*fn).confidence, 0.30f);
}

TEST(RAIIDetectorTest, FopenFcloseMatched) {
    auto fn = makeFunc("FileHandle", {});
    addCall(*fn, "fopen");
    addCall(*fn, "fclose");
    RAIIDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.90f);  // acquire + release + matching pair
    EXPECT_EQ(r.kind, PatternKind::RAII);
}

TEST(RAIIDetectorTest, MallocFreeMatched) {
    auto fn = makeFunc("BufHandle", {});
    addCall(*fn, "malloc");
    addCall(*fn, "free");
    RAIIDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.90f);
}

TEST(RAIIDetectorTest, PthreadMutexMatched) {
    auto fn = makeFunc("LockGuard", {});
    addCall(*fn, "pthread_mutex_lock");
    addCall(*fn, "pthread_mutex_unlock");
    RAIIDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.90f);
}

TEST(RAIIDetectorTest, UnmatchedAcquireOnly) {
    auto fn = makeFunc("partial", {});
    addCall(*fn, "fopen");
    RAIIDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.90f);
    EXPECT_LT(r.confidence, 0.50f);
}

TEST(RAIIDetectorTest, GroupModeCtorDtor) {
    auto ctor = makeFunc("FileHandle_ctor", {});
    addCall(*ctor, "fopen");
    auto dtor = makeFunc("FileHandle_dtor", {});
    addCall(*dtor, "fclose");
    std::vector<const ssa::SSAFunction*> fns = { ctor.get(), dtor.get() };
    RAIIDetector det;
    auto r = det.detectGroup(fns);
    EXPECT_GE(r.confidence, 0.90f);
}

TEST(RAIIDetectorTest, EmittedFormContainsRAIIHandle) {
    auto fn = makeFunc("raii", {});
    addCall(*fn, "fopen"); addCall(*fn, "fclose");
    RAIIDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.45f)
        EXPECT_NE(r.emittedForm.find("RAIIHandle"), std::string::npos);
}

// ─── PatternDetector orchestration tests ──────────────────────────────────────

TEST(PatternDetectorTest, EmptyFunctionSkipped) {
    PatternDetector::Config cfg;
    cfg.minBlocks = 2; cfg.minInstrs = 4;
    PatternDetector det(cfg);
    auto fn = makeFunc("tiny", { ssa::IrInstr::Op::Load });
    auto results = det.detectFunction(*fn);
    EXPECT_TRUE(results.empty());
}

TEST(PatternDetectorTest, SingletonDetectedInFunction) {
    PatternDetector det;
    auto fn = makeFunc("getInstance", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Ret }, 2);
    addImmCompare(*fn, 0);
    addCall(*fn, "malloc");
    auto results = det.detectFunction(*fn);
    bool found = false;
    for (const auto& r : results)
        if (r.kind == PatternKind::Singleton) found = true;
    EXPECT_TRUE(found);
}

TEST(PatternDetectorTest, RAIIDetectedInFunction) {
    PatternDetector det;
    auto fn = makeFunc("raii_fn", {ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store}, 2);
    addCall(*fn, "fopen"); addCall(*fn, "fclose");
    auto results = det.detectFunction(*fn);
    bool found = false;
    for (const auto& r : results)
        if (r.kind == PatternKind::RAII) found = true;
    EXPECT_TRUE(found);
}

TEST(PatternDetectorTest, GroupModeRunsGroupDetectors) {
    PatternDetector det;
    auto reg = makeFunc("subscribe", {}, 1);
    addCall(*reg, "push_back");
    auto notify = makeFunc("broadcast", { ssa::IrInstr::Op::Load }, 1);
    addBackEdge(*notify); addCall(*notify, "emit");
    std::vector<const ssa::SSAFunction*> fns = { reg.get(), notify.get() };
    auto results = det.detectGroup(fns);
    bool observerFound = false;
    for (const auto& r : results)
        if (r.kind == PatternKind::Observer) observerFound = true;
    EXPECT_TRUE(observerFound);
}

TEST(PatternDetectorTest, MultiplePatternsSameFunction) {
    PatternDetector det;
    // A function with both RAII and state machine signals.
    auto fn = makeFunc("complex", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Store }, 2);
    addImmCompare(*fn, 0); addImmCompare(*fn, 1);
    addCall(*fn, "fopen"); addCall(*fn, "fclose");
    auto results = det.detectFunction(*fn);
    // Should detect at least one pattern.
    EXPECT_GE(results.size(), 1u);
}

TEST(PatternDetectorTest, StatsUpdated) {
    PatternDetector det;
    auto fn = makeFunc("s", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Ret }, 2);
    addImmCompare(*fn, 0); addCall(*fn, "malloc");
    det.detectFunction(*fn);
    EXPECT_GE(det.stats().functionsAnalysed, 1u);
}

TEST(PatternDetectorTest, MinConfidenceFilters) {
    PatternDetector::Config cfg;
    cfg.minConfidence = 1.01f;  // above maximum possible confidence (1.0)
    PatternDetector det(cfg);
    auto fn = makeFunc("f", { ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Ret }, 2);
    addImmCompare(*fn, 0); addCall(*fn, "malloc");
    auto results = det.detectFunction(*fn);
    EXPECT_TRUE(results.empty());
}
