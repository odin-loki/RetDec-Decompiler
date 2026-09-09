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

/// Close a function's entry block back on itself, making it a loop header.
void makeSelfLoop(SSAFunction& fn)
{
	fn.block(0)->succs.push_back(0);
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

// The if/else chain tested substrings in the wrong order and had no case for
// the bitwise ops: InterlockedExchangeAdd hit the "Exchange" arm although it is
// a fetch-add, and InterlockedAnd/Or/Xor matched nothing and fell to an `else`
// that hardcoded FetchAdd -- which is why AtomicOp::FetchAnd/FetchOr/FetchXor
// were unreachable from Win32 input.
TEST(Win32ThreadDetector, InterlockedOpsDecodeToTheOpTheyPerform)
{
	auto fn = makeFnWithCalls(
		"atomics",
		{
			{"InterlockedExchangeAdd", 0xe100},
			{"InterlockedAnd", 0xe110},
			{"InterlockedOr", 0xe120},
			{"InterlockedXor", 0xe130},
			{"InterlockedExchange", 0xe140},
			{"InterlockedCompareExchange", 0xe150},
		});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.atomics.size(), 6u);
	EXPECT_EQ(model.atomics[0].op, AtomicOp::FetchAdd);
	EXPECT_EQ(model.atomics[1].op, AtomicOp::FetchAnd);
	EXPECT_EQ(model.atomics[2].op, AtomicOp::FetchOr);
	EXPECT_EQ(model.atomics[3].op, AtomicOp::FetchXor);
	EXPECT_EQ(model.atomics[4].op, AtomicOp::Exchange);
	EXPECT_EQ(model.atomics[5].op, AtomicOp::CompareExchange);
}

// AtomicDetector sets isMT for the GCC-builtin path; the Win32 path did not, so
// a lock-free Win32 program was reported as "Multithreaded: no" while the same
// report listed its atomics.
TEST(Win32ThreadDetector, InterlockedOpsMarkTheProgramMultithreaded)
{
	auto fn = makeFnWithCalls("ref_count", {{"InterlockedIncrement", 0xe000}});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_TRUE(model.isMT);
}

// kWin32ThreadWait was defined and referenced nowhere, so no Win32 thread was
// ever marked joined.
TEST(Win32ThreadDetector, WaitForSingleObjectJoinsTheThread)
{
	auto fn = makeFnWithCalls(
		"main",
		{
			{"CreateThread", 0xe200},
			{"WaitForSingleObject", 0xe210},
		});

	ConcurrencyModel model;
	Win32ThreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.threads.size(), 1u);
	EXPECT_TRUE(model.threads[0].isJoined);
}

// ConcurrencyDetector::analyseModule threads one model through every function
// with no reset, and the detectors paired an unlock with out.locks.back() --
// the last lock pushed anywhere in the module. An unlock in function B became
// the unlock of a lock in function A, so reversing the order the functions
// arrive in changed the model.
TEST(PthreadDetector, AnUnlockDoesNotCloseALockInAnotherFunction)
{
	auto opener = makeFnWithCalls("takes_lock", {{"pthread_mutex_lock", 0x1000}});
	auto closer = makeFnWithCalls("unrelated", {{"pthread_mutex_unlock", 0x2000}});

	ConcurrencyModel model;
	PthreadDetector det;
	det.analyseFunction(*opener, model);
	det.analyseFunction(*closer, model);

	ASSERT_EQ(model.locks.size(), 1u);
	EXPECT_EQ(model.locks[0].funcName, "takes_lock");
	EXPECT_EQ(model.locks[0].unlockCall, 0u) << "an unlock in another function is not this lock's unlock";
}

TEST(PthreadDetector, AJoinDoesNotAttachToAThreadInAnotherFunction)
{
	auto spawner = makeFnWithCalls("spawn", {{"pthread_create", 0x1000}});
	auto joiner = makeFnWithCalls("unrelated", {{"pthread_join", 0x2000}});

	ConcurrencyModel model;
	PthreadDetector det;
	det.analyseFunction(*spawner, model);
	det.analyseFunction(*joiner, model);

	ASSERT_EQ(model.threads.size(), 1u);
	EXPECT_FALSE(model.threads[0].isJoined);
}

// The pairing inside one function still works, and still picks the innermost
// lock still open rather than the last one recorded.
TEST(PthreadDetector, NestedLocksPairInnermostFirst)
{
	auto fn = makeFnWithCalls(
		"nested",
		{
			{"pthread_mutex_lock", 0x1000},
			{"pthread_mutex_lock", 0x1010},
			{"pthread_mutex_unlock", 0x1020},
			{"pthread_mutex_unlock", 0x1030},
		});

	ConcurrencyModel model;
	PthreadDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.locks.size(), 2u);
	EXPECT_EQ(model.locks[1].unlockCall, 0x1020u);
	EXPECT_EQ(model.locks[0].unlockCall, 0x1030u);
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
	makeSelfLoop(*fn);

	ConcurrencyModel model;
	SpinlockDetector det;
	det.analyseFunction(*fn, model);

	ASSERT_EQ(model.spinlocks.size(), 1u);
	EXPECT_EQ(model.spinlocks[0].funcName, "spinlock_fn");
	EXPECT_TRUE(model.isMT);
	// The header documents loopAddr as "address of the spin loop header"; it
	// used to hold the block *id*, which the emitter printed as a hex VMA and
	// suppressed entirely for block 0.
	EXPECT_EQ(model.spinlocks[0].loopAddr, 0x24000u);
}

// hasCasLoop never looked at blk->succs, so a single straight-line
// compare_exchange -- no loop anywhere -- was reported as a spinlock,
// contradicting the function's name and the pattern the header documents.
TEST(SpinlockDetector, AStraightLineCasIsNotASpinlock)
{
	auto fn = makeFnWithCalls(
		"try_once",
		{
			{"InterlockedCompareExchange", 0x24000},
		});

	ConcurrencyModel model;
	SpinlockDetector det;
	det.analyseFunction(*fn, model);

	EXPECT_TRUE(model.spinlocks.empty());
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
