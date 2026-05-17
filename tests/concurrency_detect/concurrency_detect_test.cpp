/**
 * @file tests/concurrency_detect/concurrency_detect_test.cpp
 * @brief Unit tests for the Concurrency and Synchronisation Detector.
 */

#include "retdec/concurrency_detect/concurrency_detect.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace retdec::concurrency_detect;

// ─── Helpers: build minimal SSA stubs ────────────────────────────────────────
// (Mirrors the stub structs in the .cpp file.)

namespace retdec::ssa {
struct CallInstr {
    std::string target;
    uint64_t    address = 0;
    bool        hasLockPrefix = false;
};
struct AtomicInstr {
    std::string op;
    std::string order;
    uint64_t    address = 0;
    uint64_t    varAddr = 0;
    std::string varName;
};
struct BasicBlock {
    std::vector<CallInstr>   calls;
    std::vector<AtomicInstr> atomics;
    bool hasCasLoop = false;
    bool isDCLP     = false;
};
struct SSAFunction {
    std::string              name;
    uint64_t                 address = 0;
    std::vector<BasicBlock>  blocks;
    std::vector<std::string> referencedSymbols;
};
struct SSAModule {
    std::vector<SSAFunction> functions;
};
} // namespace retdec::ssa

// ─── StdThreadDetector ────────────────────────────────────────────────────────

TEST(StdThreadDetector, DetectsStdThreadCreate) {
    retdec::ssa::SSAFunction fn;
    fn.name = "main";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"std::thread::thread", 0x1000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    StdThreadDetector det;
    det.analyseFunction(fn, model);

    EXPECT_TRUE(model.isMT);
    ASSERT_EQ(model.threads.size(), 1u);
    EXPECT_EQ(model.threads[0].lib,      ThreadLib::StdThread);
    EXPECT_EQ(model.threads[0].funcName, "main");
    EXPECT_EQ(model.threads[0].callSite, 0x1000u);
    EXPECT_EQ(model.primaryLib,          ThreadLib::StdThread);
}

TEST(StdThreadDetector, DetectsStdMutexLockGuard) {
    retdec::ssa::SSAFunction fn;
    fn.name = "worker";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"std::lock_guard::lock_guard", 0x2000});
    blk.calls.push_back({"std::lock_guard::~lock_guard", 0x2020});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    StdThreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.locks.size(), 1u);
    EXPECT_EQ(model.locks[0].kind,        MutexKind::StdMutex);
    EXPECT_TRUE(model.locks[0].isLockGuard);
    EXPECT_EQ(model.locks[0].unlockCall,  0x2020u);
}

TEST(StdThreadDetector, DetectsConditionVariable) {
    retdec::ssa::SSAFunction fn;
    fn.name = "producer";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"std::condition_variable::wait",       0x3000});
    blk.calls.push_back({"std::condition_variable::notify_all", 0x3010});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    StdThreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.condVars.size(), 2u);
    EXPECT_EQ(model.condVars[0].waitCall,  0x3000u);
    EXPECT_EQ(model.condVars[1].notifyCall,0x3010u);
    EXPECT_TRUE(model.condVars[1].notifyAll);
}

TEST(StdThreadDetector, EmptyFunctionProducesNoResults) {
    retdec::ssa::SSAFunction fn;
    fn.name = "no_sync";
    fn.blocks.emplace_back();

    ConcurrencyModel model;
    StdThreadDetector det;
    det.analyseFunction(fn, model);

    EXPECT_FALSE(model.isMT);
    EXPECT_TRUE(model.threads.empty());
    EXPECT_TRUE(model.locks.empty());
}

// ─── PthreadDetector ──────────────────────────────────────────────────────────

TEST(PthreadDetector, DetectsPthreadCreate) {
    retdec::ssa::SSAFunction fn;
    fn.name = "launch_thread";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"pthread_create", 0x4000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    PthreadDetector det;
    det.analyseFunction(fn, model);

    EXPECT_TRUE(model.isMT);
    ASSERT_EQ(model.threads.size(), 1u);
    EXPECT_EQ(model.threads[0].lib,     ThreadLib::PThread);
    EXPECT_EQ(model.primaryLib,         ThreadLib::PThread);
}

TEST(PthreadDetector, DetectsPthreadMutex) {
    retdec::ssa::SSAFunction fn;
    fn.name = "critical_section";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"pthread_mutex_lock",   0x5000});
    blk.calls.push_back({"pthread_mutex_unlock", 0x5010});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    PthreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.locks.size(), 1u);
    EXPECT_EQ(model.locks[0].kind,       MutexKind::PthreadMutex);
    EXPECT_EQ(model.locks[0].lockCall,   0x5000u);
    EXPECT_EQ(model.locks[0].unlockCall, 0x5010u);
}

TEST(PthreadDetector, DetectsRwLock) {
    retdec::ssa::SSAFunction fn;
    fn.name = "rw_func";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"pthread_rwlock_rdlock", 0x6000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    PthreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.locks.size(), 1u);
    EXPECT_EQ(model.locks[0].kind, MutexKind::PthreadRwLock);
}

TEST(PthreadDetector, DetectsSemaphore) {
    retdec::ssa::SSAFunction fn;
    fn.name = "sem_user";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"sem_wait", 0x7000});
    blk.calls.push_back({"sem_post", 0x7010});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    PthreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.semaphores.size(), 2u);
    EXPECT_EQ(model.semaphores[0].kind,     SemKind::PosixSem);
    EXPECT_EQ(model.semaphores[0].waitCall, 0x7000u);
    EXPECT_EQ(model.semaphores[1].postCall, 0x7010u);
}

TEST(PthreadDetector, DetectsBarrier) {
    retdec::ssa::SSAFunction fn;
    fn.name = "parallel_section";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"pthread_barrier_wait", 0x8000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    PthreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.barriers.size(), 1u);
    EXPECT_EQ(model.barriers[0].lib,      ThreadLib::PThread);
    EXPECT_EQ(model.barriers[0].callAddr, 0x8000u);
}

TEST(PthreadDetector, DetectsCondVar) {
    retdec::ssa::SSAFunction fn;
    fn.name = "waiter";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"pthread_cond_wait",      0x9000});
    blk.calls.push_back({"pthread_cond_broadcast", 0x9010});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    PthreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.condVars.size(), 2u);
    EXPECT_EQ(model.condVars[1].notifyAll, true);
}

// ─── Win32ThreadDetector ─────────────────────────────────────────────────────

TEST(Win32ThreadDetector, DetectsCreateThread) {
    retdec::ssa::SSAFunction fn;
    fn.name = "start_work";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"CreateThread", 0xa000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    Win32ThreadDetector det;
    det.analyseFunction(fn, model);

    EXPECT_TRUE(model.isMT);
    ASSERT_EQ(model.threads.size(), 1u);
    EXPECT_EQ(model.threads[0].lib,     ThreadLib::Win32);
    EXPECT_EQ(model.primaryLib,         ThreadLib::Win32);
}

TEST(Win32ThreadDetector, DetectsCriticalSection) {
    retdec::ssa::SSAFunction fn;
    fn.name = "cs_test";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"EnterCriticalSection", 0xb000});
    blk.calls.push_back({"LeaveCriticalSection", 0xb010});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    Win32ThreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.locks.size(), 1u);
    EXPECT_EQ(model.locks[0].kind,       MutexKind::Win32CriticalSection);
    EXPECT_EQ(model.locks[0].lockCall,   0xb000u);
    EXPECT_EQ(model.locks[0].unlockCall, 0xb010u);
}

TEST(Win32ThreadDetector, DetectsTryEnterCriticalSection) {
    retdec::ssa::SSAFunction fn;
    fn.name = "try_cs";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"TryEnterCriticalSection", 0xc000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    Win32ThreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.locks.size(), 1u);
    EXPECT_TRUE(model.locks[0].isTryLock);
}

TEST(Win32ThreadDetector, DetectsSRWLock) {
    retdec::ssa::SSAFunction fn;
    fn.name = "srw_test";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"AcquireSRWLockExclusive", 0xd000});
    blk.calls.push_back({"ReleaseSRWLockExclusive", 0xd010});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    Win32ThreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.locks.size(), 1u);
    EXPECT_EQ(model.locks[0].kind,       MutexKind::Win32SRWLock);
    EXPECT_EQ(model.locks[0].lockCall,   0xd000u);
    EXPECT_EQ(model.locks[0].unlockCall, 0xd010u);
}

TEST(Win32ThreadDetector, DetectsInterlockedIncrement) {
    retdec::ssa::SSAFunction fn;
    fn.name = "ref_count";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"InterlockedIncrement", 0xe000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    Win32ThreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.atomics.size(), 1u);
    EXPECT_EQ(model.atomics[0].op,    AtomicOp::FetchAdd);
    EXPECT_EQ(model.atomics[0].order, AtomicOrder::SeqCst);
}

TEST(Win32ThreadDetector, DetectsCondVar) {
    retdec::ssa::SSAFunction fn;
    fn.name = "cv_test";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"SleepConditionVariableCS",   0xf000});
    blk.calls.push_back({"WakeAllConditionVariable",   0xf010});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    Win32ThreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.condVars.size(), 2u);
    EXPECT_EQ(model.condVars[0].waitCall,   0xf000u);
    EXPECT_EQ(model.condVars[1].notifyCall, 0xf010u);
    EXPECT_TRUE(model.condVars[1].notifyAll);
}

TEST(Win32ThreadDetector, DetectsSemaphore) {
    retdec::ssa::SSAFunction fn;
    fn.name = "sem_test";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"CreateSemaphore",  0x10000});
    blk.calls.push_back({"ReleaseSemaphore", 0x10010});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    Win32ThreadDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.semaphores.size(), 2u);
    EXPECT_EQ(model.semaphores[0].kind, SemKind::Win32Sem);
}

// ─── AtomicDetector ──────────────────────────────────────────────────────────

TEST(AtomicDetector, DetectsIRAtomicRMW) {
    retdec::ssa::SSAFunction fn;
    fn.name = "atomic_fn";
    retdec::ssa::BasicBlock blk;
    blk.atomics.push_back({"atomicrmw", "acquire", 0x20000, 0x405000, "g_flag"});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    AtomicDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.atomics.size(), 1u);
    EXPECT_EQ(model.atomics[0].op,      AtomicOp::Exchange);
    EXPECT_EQ(model.atomics[0].order,   AtomicOrder::Acquire);
    EXPECT_EQ(model.atomics[0].varName, "g_flag");
    EXPECT_TRUE(model.isMT);
}

TEST(AtomicDetector, DetectsIRCmpxchg) {
    retdec::ssa::SSAFunction fn;
    fn.name = "cas_fn";
    retdec::ssa::BasicBlock blk;
    blk.atomics.push_back({"cmpxchg", "seq_cst", 0x21000, 0, ""});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    AtomicDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.atomics.size(), 1u);
    EXPECT_EQ(model.atomics[0].op,    AtomicOp::CompareExchange);
    EXPECT_EQ(model.atomics[0].order, AtomicOrder::SeqCst);
}

TEST(AtomicDetector, DetectsLockPrefixInstruction) {
    retdec::ssa::SSAFunction fn;
    fn.name = "lock_prefix";
    retdec::ssa::BasicBlock blk;
    retdec::ssa::CallInstr ci;
    ci.target         = "";
    ci.address        = 0x22000;
    ci.hasLockPrefix  = true;
    blk.calls.push_back(ci);
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    AtomicDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.atomics.size(), 1u);
    EXPECT_EQ(model.atomics[0].order, AtomicOrder::SeqCst);
}

TEST(AtomicDetector, FetchAddOrder) {
    retdec::ssa::SSAFunction fn;
    fn.name = "counter";
    retdec::ssa::BasicBlock blk;
    blk.atomics.push_back({"fetch_add", "relaxed", 0x23000, 0x500000, "counter"});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    AtomicDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.atomics.size(), 1u);
    EXPECT_EQ(model.atomics[0].op,    AtomicOp::FetchAdd);
    EXPECT_EQ(model.atomics[0].order, AtomicOrder::Relaxed);
}

// ─── SpinlockDetector ────────────────────────────────────────────────────────

TEST(SpinlockDetector, DetectsSpinLoop) {
    retdec::ssa::SSAFunction fn;
    fn.name = "spinlock_fn";
    retdec::ssa::BasicBlock blk;
    blk.hasCasLoop = true;
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    SpinlockDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.spinlocks.size(), 1u);
    EXPECT_EQ(model.spinlocks[0].funcName, "spinlock_fn");
    EXPECT_TRUE(model.isMT);
}

TEST(SpinlockDetector, DetectsDCLPPattern) {
    retdec::ssa::SSAFunction fn;
    fn.name = "singleton";
    retdec::ssa::BasicBlock blk;
    blk.hasCasLoop = true;
    blk.isDCLP     = true;
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    SpinlockDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.spinlocks.size(), 1u);
    EXPECT_TRUE(model.spinlocks[0].isDCLP);
}

// ─── OpenMPDetector ──────────────────────────────────────────────────────────

TEST(OpenMPDetector, DetectsGOMPParallel) {
    retdec::ssa::SSAFunction fn;
    fn.name = "omp_work";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"GOMP_parallel", 0x30000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    OpenMPDetector det;
    det.analyseFunction(fn, model);

    EXPECT_TRUE(model.isMT);
    ASSERT_EQ(model.ompRegions.size(), 1u);
    EXPECT_EQ(model.ompRegions[0].kind,     "parallel");
    EXPECT_EQ(model.ompRegions[0].funcName, "omp_work");
    EXPECT_EQ(model.primaryLib, ThreadLib::OpenMP);
}

TEST(OpenMPDetector, DetectsKMPCForkCall) {
    retdec::ssa::SSAFunction fn;
    fn.name = "omp_kmpc";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"__kmpc_fork_call", 0x31000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    OpenMPDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.ompRegions.size(), 1u);
    EXPECT_EQ(model.ompRegions[0].kind, "parallel");
}

TEST(OpenMPDetector, DetectsBarrier) {
    retdec::ssa::SSAFunction fn;
    fn.name = "omp_bar";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"GOMP_barrier", 0x32000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    OpenMPDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.barriers.size(), 1u);
    EXPECT_EQ(model.barriers[0].lib, ThreadLib::OpenMP);
}

// ─── TBBDetector ─────────────────────────────────────────────────────────────

TEST(TBBDetector, DetectsParallelFor) {
    retdec::ssa::SSAFunction fn;
    fn.name = "tbb_work";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"tbb::parallel_for", 0x40000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    TBBDetector det;
    det.analyseFunction(fn, model);

    EXPECT_TRUE(model.isMT);
    ASSERT_EQ(model.tbbPatterns.size(), 1u);
    EXPECT_EQ(model.tbbPatterns[0].kind, "parallel_for");
    EXPECT_EQ(model.primaryLib, ThreadLib::TBB);
}

TEST(TBBDetector, DetectsTaskGroup) {
    retdec::ssa::SSAFunction fn;
    fn.name = "tg_test";
    retdec::ssa::BasicBlock blk;
    blk.calls.push_back({"tbb::task_group::run", 0x41000});
    fn.blocks.push_back(blk);

    ConcurrencyModel model;
    TBBDetector det;
    det.analyseFunction(fn, model);

    ASSERT_EQ(model.tbbPatterns.size(), 1u);
    EXPECT_EQ(model.tbbPatterns[0].kind, "task_group");
}

// ─── ConcurrencyDetector (orchestrator) ──────────────────────────────────────

TEST(ConcurrencyDetector, AnalysesModuleWithMultipleFunctions) {
    retdec::ssa::SSAModule mod;

    retdec::ssa::SSAFunction f1;
    f1.name = "create_thread";
    retdec::ssa::BasicBlock b1;
    b1.calls.push_back({"pthread_create", 0x1000});
    f1.blocks.push_back(b1);
    mod.functions.push_back(f1);

    retdec::ssa::SSAFunction f2;
    f2.name = "do_work";
    retdec::ssa::BasicBlock b2;
    b2.calls.push_back({"pthread_mutex_lock",   0x2000});
    b2.calls.push_back({"pthread_mutex_unlock", 0x2010});
    f2.blocks.push_back(b2);
    mod.functions.push_back(f2);

    ConcurrencyDetector det;
    auto model = det.analyseModule(mod);

    EXPECT_TRUE(model.isMT);
    EXPECT_EQ(model.primaryLib, ThreadLib::PThread);
    EXPECT_GE(model.threads.size(), 1u);
    EXPECT_GE(model.locks.size(),   1u);
}

TEST(ConcurrencyDetector, EmptyModuleNotMT) {
    retdec::ssa::SSAModule mod;
    ConcurrencyDetector det;
    auto model = det.analyseModule(mod);
    EXPECT_FALSE(model.isMT);
}

TEST(ConcurrencyDetector, MixedLibraries) {
    retdec::ssa::SSAModule mod;

    retdec::ssa::SSAFunction f1;
    f1.name = "a";
    retdec::ssa::BasicBlock b1;
    b1.calls.push_back({"std::thread::thread", 0x100});
    f1.blocks.push_back(b1);

    retdec::ssa::SSAFunction f2;
    f2.name = "b";
    retdec::ssa::BasicBlock b2;
    b2.atomics.push_back({"fetch_add", "seq_cst", 0x200, 0, "x"});
    f2.blocks.push_back(b2);

    mod.functions.push_back(f1);
    mod.functions.push_back(f2);

    ConcurrencyDetector det;
    auto model = det.analyseModule(mod);

    EXPECT_TRUE(model.isMT);
    EXPECT_GE(model.threads.size(), 1u);
    EXPECT_GE(model.atomics.size(), 1u);
}

// ─── ConcurrencyModel::merge ─────────────────────────────────────────────────

TEST(ConcurrencyModel, MergeEmpty) {
    ConcurrencyModel a, b;
    a.merge(b);
    EXPECT_FALSE(a.isMT);
}

TEST(ConcurrencyModel, MergePreservesData) {
    ConcurrencyModel a, b;
    ThreadInfo ti; ti.lib = ThreadLib::PThread; ti.funcName = "f";
    b.threads.push_back(ti);
    b.isMT = true;
    b.primaryLib = ThreadLib::PThread;
    a.merge(b);
    ASSERT_EQ(a.threads.size(), 1u);
    EXPECT_EQ(a.threads[0].lib, ThreadLib::PThread);
    EXPECT_TRUE(a.isMT);
}

// ─── ConcurrencyEmitter ───────────────────────────────────────────────────────

TEST(ConcurrencyEmitter, EmitEmptyModel) {
    ConcurrencyModel model;
    ConcurrencyEmitter emitter;
    auto s = emitter.emit(model);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("Multithreaded: no"), std::string::npos);
}

TEST(ConcurrencyEmitter, EmitWithThreads) {
    ConcurrencyModel model;
    model.isMT = true;
    model.primaryLib = ThreadLib::PThread;
    ThreadInfo ti;
    ti.lib      = ThreadLib::PThread;
    ti.funcName = "main";
    ti.callSite = 0x1234;
    ti.isJoined = true;
    model.threads.push_back(ti);

    ConcurrencyEmitter emitter;
    auto s = emitter.emit(model);
    EXPECT_NE(s.find("pthreads"),   std::string::npos);
    EXPECT_NE(s.find("main"),       std::string::npos);
    EXPECT_NE(s.find("joined"),     std::string::npos);
    EXPECT_NE(s.find("1234"),       std::string::npos);
}

TEST(ConcurrencyEmitter, EmitWithAtomics) {
    ConcurrencyModel model;
    model.isMT = true;
    AtomicInfo ai;
    ai.op      = AtomicOp::FetchAdd;
    ai.order   = AtomicOrder::Relaxed;
    ai.funcName= "counter_fn";
    ai.address = 0xabcd;
    model.atomics.push_back(ai);

    ConcurrencyEmitter emitter;
    auto s = emitter.emit(model);
    EXPECT_NE(s.find("fetch_add"),              std::string::npos);
    EXPECT_NE(s.find("memory_order_relaxed"),   std::string::npos);
    EXPECT_NE(s.find("counter_fn"),             std::string::npos);
}

TEST(ConcurrencyEmitter, EmitWithOpenMP) {
    ConcurrencyModel model;
    model.isMT      = true;
    model.primaryLib= ThreadLib::OpenMP;
    OpenMPRegion r;
    r.kind     = "parallel";
    r.funcName = "omp_fn";
    r.forkCall = 0x5000;
    model.ompRegions.push_back(r);

    ConcurrencyEmitter emitter;
    auto s = emitter.emit(model);
    EXPECT_NE(s.find("#pragma omp parallel"), std::string::npos);
    EXPECT_NE(s.find("omp_fn"),               std::string::npos);
}

TEST(ConcurrencyEmitter, EmitWithSpinlock) {
    ConcurrencyModel model;
    model.isMT = true;
    SpinlockInfo si;
    si.funcName = "spin_fn";
    si.loopAddr = 0x6000;
    si.isDCLP   = true;
    model.spinlocks.push_back(si);

    ConcurrencyEmitter emitter;
    auto s = emitter.emit(model);
    EXPECT_NE(s.find("Spinlock"), std::string::npos);
    EXPECT_NE(s.find("DCLP"),     std::string::npos);
    EXPECT_NE(s.find("spin_fn"),  std::string::npos);
}
