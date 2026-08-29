/**
 * @file tests/concurrency_detect/concurrency_detect_test.cpp
 * @brief Unit tests for the Concurrency and Synchronisation Detector.
 *
 * Detectors match callee names (and Op::Lock) on real SSA instructions.
 * Atomic varName/varAddr are not populated from these fixtures.
 */

#include "retdec/concurrency_detect/concurrency_detect.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

using namespace retdec::concurrency_detect;
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

// ─── StdThreadDetector ────────────────────────────────────────────────────────

TEST(StdThreadDetector, DetectsStdThreadCreate) {
	auto fn = makeFnWithCalls("main", {{"std::thread::thread", 0x1000}});

	ConcurrencyModel model;
	StdThreadDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_TRUE(model.isMT);
	ASSERT_EQ(model.threads.size(), 1u);
	EXPECT_EQ(model.threads[0].lib,      ThreadLib::StdThread);
	EXPECT_EQ(model.threads[0].funcName, "main");
	EXPECT_EQ(model.threads[0].callSite, 0x1000u);
	EXPECT_EQ(model.primaryLib,          ThreadLib::StdThread);
}

TEST(StdThreadDetector, DetectsStdMutexLockGuard) {
	auto fn = makeFnWithCalls("worker", {
			{"std::lock_guard::lock_guard", 0x2000},
			{"std::lock_guard::~lock_guard", 0x2020},
	});

	ConcurrencyModel model;
	StdThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.locks.size(), 1u);
	EXPECT_EQ(model.locks[0].kind,        MutexKind::StdMutex);
	EXPECT_TRUE(model.locks[0].isLockGuard);
	EXPECT_EQ(model.locks[0].unlockCall,  0x2020u);
}

TEST(StdThreadDetector, DetectsConditionVariable) {
	auto fn = makeFnWithCalls("producer", {
			{"std::condition_variable::wait",       0x3000},
			{"std::condition_variable::notify_all", 0x3010},
	});

	ConcurrencyModel model;
	StdThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.condVars.size(), 2u);
	EXPECT_EQ(model.condVars[0].waitCall,  0x3000u);
	EXPECT_EQ(model.condVars[1].notifyCall,0x3010u);
	EXPECT_TRUE(model.condVars[1].notifyAll);
}

TEST(StdThreadDetector, EmptyFunctionProducesNoResults) {
	auto fn = makeFnWithCalls("no_sync", {});

	ConcurrencyModel model;
	StdThreadDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_FALSE(model.isMT);
	EXPECT_TRUE(model.threads.empty());
	EXPECT_TRUE(model.locks.empty());
}

// ─── PthreadDetector ──────────────────────────────────────────────────────────

TEST(PthreadDetector, DetectsPthreadCreate) {
	auto fn = makeFnWithCalls("launch_thread", {{"pthread_create", 0x4000}});

	ConcurrencyModel model;
	PthreadDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_TRUE(model.isMT);
	ASSERT_EQ(model.threads.size(), 1u);
	EXPECT_EQ(model.threads[0].lib,     ThreadLib::PThread);
	EXPECT_EQ(model.primaryLib,         ThreadLib::PThread);
}

TEST(PthreadDetector, DetectsPthreadMutex) {
	auto fn = makeFnWithCalls("critical_section", {
			{"pthread_mutex_lock",   0x5000},
			{"pthread_mutex_unlock", 0x5010},
	});

	ConcurrencyModel model;
	PthreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.locks.size(), 1u);
	EXPECT_EQ(model.locks[0].kind,       MutexKind::PthreadMutex);
	EXPECT_EQ(model.locks[0].lockCall,   0x5000u);
	EXPECT_EQ(model.locks[0].unlockCall, 0x5010u);
}

TEST(PthreadDetector, DetectsRwLock) {
	auto fn = makeFnWithCalls("rw_func", {{"pthread_rwlock_rdlock", 0x6000}});

	ConcurrencyModel model;
	PthreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.locks.size(), 1u);
	EXPECT_EQ(model.locks[0].kind, MutexKind::PthreadRwLock);
}

TEST(PthreadDetector, DetectsSemaphore) {
	auto fn = makeFnWithCalls("sem_user", {
			{"sem_wait", 0x7000},
			{"sem_post", 0x7010},
	});

	ConcurrencyModel model;
	PthreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.semaphores.size(), 2u);
	EXPECT_EQ(model.semaphores[0].kind,     SemKind::PosixSem);
	EXPECT_EQ(model.semaphores[0].waitCall, 0x7000u);
	EXPECT_EQ(model.semaphores[1].postCall, 0x7010u);
}

TEST(PthreadDetector, DetectsBarrier) {
	auto fn = makeFnWithCalls("parallel_section", {{"pthread_barrier_wait", 0x8000}});

	ConcurrencyModel model;
	PthreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.barriers.size(), 1u);
	EXPECT_EQ(model.barriers[0].lib,      ThreadLib::PThread);
	EXPECT_EQ(model.barriers[0].callAddr, 0x8000u);
}

TEST(PthreadDetector, DetectsCondVar) {
	auto fn = makeFnWithCalls("waiter", {
			{"pthread_cond_wait",      0x9000},
			{"pthread_cond_broadcast", 0x9010},
	});

	ConcurrencyModel model;
	PthreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.condVars.size(), 2u);
	EXPECT_EQ(model.condVars[1].notifyAll, true);
}

// ─── Win32ThreadDetector ─────────────────────────────────────────────────────

TEST(Win32ThreadDetector, DetectsCreateThread) {
	auto fn = makeFnWithCalls("start_work", {{"CreateThread", 0xa000}});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_TRUE(model.isMT);
	ASSERT_EQ(model.threads.size(), 1u);
	EXPECT_EQ(model.threads[0].lib,     ThreadLib::Win32);
	EXPECT_EQ(model.primaryLib,         ThreadLib::Win32);
}

TEST(Win32ThreadDetector, DetectsCriticalSection) {
	auto fn = makeFnWithCalls("cs_test", {
			{"EnterCriticalSection", 0xb000},
			{"LeaveCriticalSection", 0xb010},
	});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.locks.size(), 1u);
	EXPECT_EQ(model.locks[0].kind,       MutexKind::Win32CriticalSection);
	EXPECT_EQ(model.locks[0].lockCall,   0xb000u);
	EXPECT_EQ(model.locks[0].unlockCall, 0xb010u);
}

TEST(Win32ThreadDetector, DetectsTryEnterCriticalSection) {
	auto fn = makeFnWithCalls("try_cs", {{"TryEnterCriticalSection", 0xc000}});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.locks.size(), 1u);
	EXPECT_TRUE(model.locks[0].isTryLock);
}

TEST(Win32ThreadDetector, DetectsSRWLock) {
	auto fn = makeFnWithCalls("srw_test", {
			{"AcquireSRWLockExclusive", 0xd000},
			{"ReleaseSRWLockExclusive", 0xd010},
	});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.locks.size(), 1u);
	EXPECT_EQ(model.locks[0].kind,       MutexKind::Win32SRWLock);
	EXPECT_EQ(model.locks[0].lockCall,   0xd000u);
	EXPECT_EQ(model.locks[0].unlockCall, 0xd010u);
}

TEST(Win32ThreadDetector, DetectsInterlockedIncrement) {
	auto fn = makeFnWithCalls("ref_count", {{"InterlockedIncrement", 0xe000}});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.atomics.size(), 1u);
	EXPECT_EQ(model.atomics[0].op,    AtomicOp::FetchAdd);
	EXPECT_EQ(model.atomics[0].order, AtomicOrder::SeqCst);
}

TEST(Win32ThreadDetector, DetectsCondVar) {
	auto fn = makeFnWithCalls("cv_test", {
			{"SleepConditionVariableCS",   0xf000},
			{"WakeAllConditionVariable",   0xf010},
	});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.condVars.size(), 2u);
	EXPECT_EQ(model.condVars[0].waitCall,   0xf000u);
	EXPECT_EQ(model.condVars[1].notifyCall, 0xf010u);
	EXPECT_TRUE(model.condVars[1].notifyAll);
}

TEST(Win32ThreadDetector, DetectsSemaphore) {
	auto fn = makeFnWithCalls("sem_test", {
			{"CreateSemaphore",  0x10000},
			{"ReleaseSemaphore", 0x10010},
	});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.semaphores.size(), 2u);
	EXPECT_EQ(model.semaphores[0].kind, SemKind::Win32Sem);
}

// ─── AtomicDetector ──────────────────────────────────────────────────────────

TEST(AtomicDetector, DetectsIRAtomicRMW) {
	auto fn = makeFnWithCalls("atomic_fn", {
			{"__atomic_exchange_acquire", 0x20000},
	});

	ConcurrencyModel model;
	AtomicDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.atomics.size(), 1u);
	EXPECT_EQ(model.atomics[0].op,      AtomicOp::Exchange);
	EXPECT_EQ(model.atomics[0].order,   AtomicOrder::Acquire);
	EXPECT_TRUE(model.atomics[0].varName.empty());
	EXPECT_TRUE(model.isMT);
}

TEST(AtomicDetector, DetectsIRCmpxchg) {
	auto fn = makeFnWithCalls("cas_fn", {
			{"__atomic_compare_exchange", 0x21000},
	});

	ConcurrencyModel model;
	AtomicDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.atomics.size(), 1u);
	EXPECT_EQ(model.atomics[0].op,    AtomicOp::CompareExchange);
	EXPECT_EQ(model.atomics[0].order, AtomicOrder::SeqCst);
}

TEST(AtomicDetector, DetectsLockPrefixInstruction) {
	auto fn = std::make_unique<SSAFunction>("lock_prefix");
	fn->addBlock("entry");
	fn->addInstr(0, IrInstr::Op::Lock, 0x22000);

	ConcurrencyModel model;
	AtomicDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.atomics.size(), 1u);
	EXPECT_EQ(model.atomics[0].order, AtomicOrder::SeqCst);
}

TEST(AtomicDetector, FetchAddOrder) {
	auto fn = makeFnWithCalls("counter", {
			{"__atomic_fetch_add_relaxed", 0x23000},
	});

	ConcurrencyModel model;
	AtomicDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.atomics.size(), 1u);
	EXPECT_EQ(model.atomics[0].op,    AtomicOp::FetchAdd);
	EXPECT_EQ(model.atomics[0].order, AtomicOrder::Relaxed);
}

// ─── SpinlockDetector ────────────────────────────────────────────────────────

TEST(SpinlockDetector, DetectsSpinLoop) {
	auto fn = makeFnWithCalls("spinlock_fn", {
			{"InterlockedCompareExchange", 0x24000},
	});

	ConcurrencyModel model;
	SpinlockDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.spinlocks.size(), 1u);
	EXPECT_EQ(model.spinlocks[0].funcName, "spinlock_fn");
	EXPECT_TRUE(model.isMT);
}

TEST(SpinlockDetector, DetectsDCLPPattern) {
	auto fn = makeFnWithCalls("singleton", {
			{"__atomic_load", 0x25000},
			{"pthread_mutex_lock", 0x25010},
			{"__atomic_compare_exchange", 0x25020},
	});

	ConcurrencyModel model;
	SpinlockDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.spinlocks.size(), 1u);
	EXPECT_TRUE(model.spinlocks[0].isDCLP);
}

// ─── OpenMPDetector ──────────────────────────────────────────────────────────

TEST(OpenMPDetector, DetectsGOMPParallel) {
	auto fn = makeFnWithCalls("omp_work", {{"GOMP_parallel", 0x30000}});

	ConcurrencyModel model;
	OpenMPDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_TRUE(model.isMT);
	ASSERT_EQ(model.ompRegions.size(), 1u);
	EXPECT_EQ(model.ompRegions[0].kind,     "parallel");
	EXPECT_EQ(model.ompRegions[0].funcName, "omp_work");
	EXPECT_EQ(model.primaryLib, ThreadLib::OpenMP);
}

TEST(OpenMPDetector, DetectsKMPCForkCall) {
	auto fn = makeFnWithCalls("omp_kmpc", {{"__kmpc_fork_call", 0x31000}});

	ConcurrencyModel model;
	OpenMPDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.ompRegions.size(), 1u);
	EXPECT_EQ(model.ompRegions[0].kind, "parallel");
}

TEST(OpenMPDetector, DetectsBarrier) {
	auto fn = makeFnWithCalls("omp_bar", {{"GOMP_barrier", 0x32000}});

	ConcurrencyModel model;
	OpenMPDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.barriers.size(), 1u);
	EXPECT_EQ(model.barriers[0].lib, ThreadLib::OpenMP);
}

// ─── TBBDetector ─────────────────────────────────────────────────────────────

TEST(TBBDetector, DetectsParallelFor) {
	auto fn = makeFnWithCalls("tbb_work", {{"tbb::parallel_for", 0x40000}});

	ConcurrencyModel model;
	TBBDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_TRUE(model.isMT);
	ASSERT_EQ(model.tbbPatterns.size(), 1u);
	EXPECT_EQ(model.tbbPatterns[0].kind, "parallel_for");
	EXPECT_EQ(model.primaryLib, ThreadLib::TBB);
}

TEST(TBBDetector, DetectsTaskGroup) {
	auto fn = makeFnWithCalls("tg_test", {{"tbb::task_group::run", 0x41000}});

	ConcurrencyModel model;
	TBBDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.tbbPatterns.size(), 1u);
	EXPECT_EQ(model.tbbPatterns[0].kind, "task_group");
}

// ─── ConcurrencyDetector (orchestrator) ──────────────────────────────────────

TEST(ConcurrencyDetector, AnalysesModuleWithMultipleFunctions) {
	SSAModule mod;

	{
		SSAFunction* fn = mod.addFunction("create_thread");
		fn->addBlock("entry");
		IrInstr* c = fn->addInstr(0, IrInstr::Op::Call, 0x1000);
		c->calleeName = "pthread_create";
	}

	{
		SSAFunction* fn = mod.addFunction("do_work");
		fn->addBlock("entry");
		IrInstr* lock = fn->addInstr(0, IrInstr::Op::Call, 0x2000);
		lock->calleeName = "pthread_mutex_lock";
		IrInstr* unlock = fn->addInstr(0, IrInstr::Op::Call, 0x2010);
		unlock->calleeName = "pthread_mutex_unlock";
	}

	ConcurrencyDetector det;
	auto model = det.analyseModule(mod);

	EXPECT_TRUE(model.isMT);
	EXPECT_EQ(model.primaryLib, ThreadLib::PThread);
	EXPECT_GE(model.threads.size(), 1u);
	EXPECT_GE(model.locks.size(),   1u);
}

TEST(ConcurrencyDetector, EmptyModuleNotMT) {
	SSAModule mod;
	ConcurrencyDetector det;
	auto model = det.analyseModule(mod);
	EXPECT_FALSE(model.isMT);
}

TEST(ConcurrencyDetector, MixedLibraries) {
	SSAModule mod;

	{
		SSAFunction* fn = mod.addFunction("a");
		fn->addBlock("entry");
		IrInstr* t = fn->addInstr(0, IrInstr::Op::Call, 0x100);
		t->calleeName = "std::thread::thread";
	}

	{
		SSAFunction* fn = mod.addFunction("b");
		fn->addBlock("entry");
		IrInstr* add = fn->addInstr(0, IrInstr::Op::Call, 0x200);
		add->calleeName = "__atomic_fetch_add";
	}

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
