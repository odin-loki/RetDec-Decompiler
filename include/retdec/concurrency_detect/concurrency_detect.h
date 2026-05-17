/**
 * @file include/retdec/concurrency_detect/concurrency_detect.h
 * @brief Concurrency and Synchronisation Detector — Stage 39.
 *
 * Identifies compiled concurrency primitives and thread management APIs in
 * binary code, then reconstructs a ConcurrencyModel describing:
 *   - Threading libraries used (std::thread, pthreads, Win32, OpenMP, TBB)
 *   - Mutex / lock guard patterns
 *   - Atomic variable accesses
 *   - Condition variable usage
 *   - Semaphore / event / barrier patterns
 *   - Spinlock patterns
 *   - Thread pool signatures
 *
 * ## Detection Strategies
 *
 * ### std::thread / std::mutex (C++11 standard library)
 *
 *   Symbol matching:
 *     `std::thread::thread`, `std::thread::join`, `std::thread::detach`,
 *     `std::mutex::lock`, `std::mutex::unlock`, `std::mutex::try_lock`,
 *     `std::unique_lock::unique_lock`, `std::lock_guard::lock_guard`,
 *     `std::condition_variable::wait`, `std::condition_variable::notify_one/all`,
 *     `std::atomic::load`, `std::atomic::store`, `std::atomic::fetch_add`, etc.
 *
 *   IR pattern: LOCK prefix on XCHG/CMPXCHG instructions → atomic RMW.
 *   Memory ordering operand values 0–5 (relaxed→seq_cst) on atomic ops.
 *
 * ### POSIX pthreads
 *
 *   Symbol matching:
 *     `pthread_create`, `pthread_join`, `pthread_detach`,
 *     `pthread_mutex_init`, `pthread_mutex_lock`, `pthread_mutex_unlock`,
 *     `pthread_mutex_trylock`, `pthread_mutex_destroy`,
 *     `pthread_cond_init`, `pthread_cond_wait`, `pthread_cond_signal`,
 *     `pthread_cond_broadcast`, `pthread_cond_timedwait`,
 *     `pthread_rwlock_rdlock`, `pthread_rwlock_wrlock`,
 *     `pthread_barrier_wait`,
 *     `sem_init`, `sem_wait`, `sem_post`, `sem_timedwait`.
 *
 *   Struct layouts: `pthread_mutex_t` (4–40 bytes depending on impl),
 *     `pthread_t` (unsigned long or pointer-sized).
 *
 * ### Win32 synchronisation
 *
 *   Symbol matching:
 *     `CreateThread`, `WaitForSingleObject`, `WaitForMultipleObjects`,
 *     `InitializeCriticalSection`, `EnterCriticalSection`, `LeaveCriticalSection`,
 *     `TryEnterCriticalSection`, `DeleteCriticalSection`,
 *     `CreateMutex`, `ReleaseMutex`, `CreateSemaphore`, `ReleaseSemaphore`,
 *     `CreateEvent`, `SetEvent`, `ResetEvent`, `PulseEvent`,
 *     `InitOnceExecuteOnce`, `AcquireSRWLockExclusive`, `ReleaseSRWLockExclusive`,
 *     `InitializeConditionVariable`, `SleepConditionVariableCS`,
 *     `WakeConditionVariable`, `WakeAllConditionVariable`,
 *     `InterlockedIncrement`, `InterlockedDecrement`, `InterlockedExchange`,
 *     `InterlockedCompareExchange`, `InterlockedAdd`.
 *
 *   CRITICAL_SECTION struct layout: first field `DebugInfo` pointer,
 *     `LockCount` (int32), `RecursionCount` (int32).
 *
 * ### Atomic operations (GCC / Clang builtins → compiler intrinsics)
 *
 *   LLVM IR: `atomicrmw`, `cmpxchg`, `fence` instructions.
 *   x86: `LOCK XADD`, `LOCK CMPXCHG`, `XCHG` (implicit LOCK).
 *   ARM: `LDREX`/`STREX` pairs, `DMB`/`DSB` barriers.
 *   GCC builtins: `__atomic_load_n`, `__atomic_store_n`, `__atomic_exchange_n`,
 *     `__atomic_compare_exchange_n`, `__atomic_fetch_add/sub/and/or/xor/nand`.
 *
 * ### OpenMP
 *
 *   Symbol matching: `GOMP_*` (GCC) or `__kmpc_*` (Intel/LLVM-OpenMP):
 *     `GOMP_parallel`, `GOMP_parallel_end`, `GOMP_barrier`,
 *     `GOMP_critical_start`, `GOMP_critical_end`, `GOMP_atomic_start/end`,
 *     `__kmpc_fork_call`, `__kmpc_barrier`, `__kmpc_critical`,
 *     `__kmpc_atomic_fixed4_add`, `__kmpc_omp_task`.
 *
 * ### Intel TBB
 *
 *   Symbol matching: `tbb::task_group`, `tbb::parallel_for`,
 *     `tbb::queuing_mutex`, `tbb::spin_mutex`, `tbb::reader_writer_lock`,
 *     `tbb::atomic`, `tbb::concurrent_queue`, `tbb::concurrent_vector`.
 *
 * ### Spinlock pattern detection (lock-free code)
 *
 *   Pattern: tight loop containing `CMPXCHG` or `XCHG` (LOCK prefix) on
 *     a shared flag until exchange succeeds — no system call, no sleep.
 *   IR: `cmpxchg` result `success` feeds a branch back to the same block.
 *
 * ### Double-checked locking (DCLP)
 *
 *   Pattern: load(flag, relaxed) → branch → lock() → load(flag, acquire)
 *     → if still 0: initialise; flag.store(1, release) → unlock().
 *
 * ## Output
 *
 *   ConcurrencyModel — top-level result containing:
 *     ThreadInfo   — detected thread creation points
 *     LockInfo     — mutex / lock guard usages
 *     AtomicInfo   — atomic variable accesses and orderings
 *     CondVarInfo  — condition variable wait/signal pairs
 *     SemaphoreInfo— semaphore up/down pairs
 *     BarrierInfo  — barrier synchronisation points
 *     SpinlockInfo — spin-loop locking patterns
 *
 *   ConcurrencyEmitter — emits a C++ header summarising the model.
 */

#ifndef RETDEC_CONCURRENCY_DETECT_H
#define RETDEC_CONCURRENCY_DETECT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retdec::ssa {
struct SSAFunction;
struct SSAModule;
}

namespace retdec::concurrency_detect {

// ─── Enumerations ─────────────────────────────────────────────────────────────

enum class ThreadLib {
    Unknown,
    StdThread,   ///< C++11 std::thread
    PThread,     ///< POSIX pthreads
    Win32,       ///< Win32 CreateThread
    OpenMP,      ///< GOMP_* / __kmpc_*
    TBB,         ///< Intel TBB
    Custom,      ///< Detected thread pool with no matching library
};

enum class MutexKind {
    Unknown,
    StdMutex,          ///< std::mutex
    StdRecursiveMutex, ///< std::recursive_mutex
    StdSharedMutex,    ///< std::shared_mutex (rwlock)
    PthreadMutex,
    PthreadRwLock,
    Win32CriticalSection,
    Win32Mutex,
    Win32SRWLock,
    OmpCritical,
    TBBMutex,
    Spinlock,          ///< lock-free CAS loop
};

enum class AtomicOrder {
    Relaxed,   ///< memory_order_relaxed
    Consume,   ///< memory_order_consume
    Acquire,   ///< memory_order_acquire
    Release,   ///< memory_order_release
    AcqRel,    ///< memory_order_acq_rel
    SeqCst,    ///< memory_order_seq_cst
    Unknown,
};

enum class AtomicOp {
    Load, Store, Exchange, CompareExchange,
    FetchAdd, FetchSub, FetchAnd, FetchOr, FetchXor, FetchNand,
    Fence,
    Unknown,
};

enum class CondVarLib {
    Unknown,
    StdCondVar,
    Pthread,
    Win32CondVar,
};

enum class SemKind {
    Unknown,
    PosixSem,    ///< sem_init / sem_wait / sem_post
    Win32Sem,    ///< CreateSemaphore / WaitForSingleObject
};

// ─── Detection evidence structures ───────────────────────────────────────────

struct ThreadInfo {
    ThreadLib     lib       = ThreadLib::Unknown;
    std::string   funcName;          ///< surrounding function
    uint64_t      callSite  = 0;     ///< address of the create call
    std::string   threadFunc;        ///< name of the thread entry function
    bool          isJoined  = false;
    bool          isDetached = false;
};

struct LockInfo {
    MutexKind   kind        = MutexKind::Unknown;
    std::string funcName;
    uint64_t    lockAddr    = 0;    ///< address of mutex variable (if recoverable)
    uint64_t    lockCall    = 0;    ///< address of lock() / Enter / acquire call
    uint64_t    unlockCall  = 0;    ///< address of unlock() / Leave / release call
    bool        isLockGuard = false; ///< wrapped in RAII guard
    bool        isTryLock   = false;
};

struct AtomicInfo {
    AtomicOp    op          = AtomicOp::Unknown;
    AtomicOrder order       = AtomicOrder::Unknown;
    std::string funcName;
    uint64_t    address     = 0;    ///< instruction address
    uint64_t    varAddr     = 0;    ///< address of the atomic variable
    std::string varName;
};

struct CondVarInfo {
    CondVarLib  lib         = CondVarLib::Unknown;
    std::string funcName;
    uint64_t    waitCall    = 0;
    uint64_t    notifyCall  = 0;
    bool        notifyAll   = false;
};

struct SemaphoreInfo {
    SemKind     kind        = SemKind::Unknown;
    std::string funcName;
    uint64_t    waitCall    = 0;
    uint64_t    postCall    = 0;
    int         initValue   = -1;   ///< -1 = unknown
};

struct BarrierInfo {
    ThreadLib   lib         = ThreadLib::Unknown;
    std::string funcName;
    uint64_t    callAddr    = 0;
};

struct SpinlockInfo {
    std::string funcName;
    uint64_t    loopAddr    = 0;    ///< address of the spin loop header
    uint64_t    varAddr     = 0;    ///< flag / counter address
    bool        isDCLP      = false; ///< double-checked locking pattern
};

struct OpenMPRegion {
    std::string kind;               ///< "parallel", "critical", "atomic", "barrier"
    std::string funcName;
    uint64_t    forkCall    = 0;
};

struct TBBPattern {
    std::string kind;               ///< "parallel_for", "task_group", "mutex"
    std::string funcName;
    uint64_t    callAddr    = 0;
};

// ─── ConcurrencyModel ─────────────────────────────────────────────────────────

/**
 * @brief Full concurrency model for the analysed binary.
 */
struct ConcurrencyModel {
    std::vector<ThreadInfo>    threads;
    std::vector<LockInfo>      locks;
    std::vector<AtomicInfo>    atomics;
    std::vector<CondVarInfo>   condVars;
    std::vector<SemaphoreInfo> semaphores;
    std::vector<BarrierInfo>   barriers;
    std::vector<SpinlockInfo>  spinlocks;
    std::vector<OpenMPRegion>  ompRegions;
    std::vector<TBBPattern>    tbbPatterns;

    ThreadLib primaryLib = ThreadLib::Unknown;   ///< dominant threading library
    bool      isMT       = false;                ///< true if any concurrency detected

    void merge(const ConcurrencyModel& other);
};

// ─── Detector interface ───────────────────────────────────────────────────────

/**
 * @brief Interface for per-function concurrency detectors.
 */
class IConcurrencyDetector {
public:
    virtual ~IConcurrencyDetector() = default;
    virtual void analyseFunction(const retdec::ssa::SSAFunction& fn,
                                 ConcurrencyModel& out) = 0;
};

// ─── Concrete detectors ───────────────────────────────────────────────────────

class StdThreadDetector   : public IConcurrencyDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         ConcurrencyModel& out) override;
};

class PthreadDetector     : public IConcurrencyDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         ConcurrencyModel& out) override;
};

class Win32ThreadDetector : public IConcurrencyDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         ConcurrencyModel& out) override;
};

class AtomicDetector      : public IConcurrencyDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         ConcurrencyModel& out) override;
};

class SpinlockDetector    : public IConcurrencyDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         ConcurrencyModel& out) override;
};

class OpenMPDetector      : public IConcurrencyDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         ConcurrencyModel& out) override;
};

class TBBDetector         : public IConcurrencyDetector {
public:
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         ConcurrencyModel& out) override;
};

// ─── Emitter ─────────────────────────────────────────────────────────────────

/**
 * @brief Emits a C++ header summarising the detected concurrency model.
 *
 * Example output:
 *
 *   // === Concurrency Model ===
 *   // Threading library: pthreads
 *   //
 *   // Threads:
 *   //   pthread_create @ 0x401234 → thread_func_0
 *   //
 *   // Mutexes:
 *   //   pthread_mutex_t mutex_0x404010; // lock@0x401260, unlock@0x401290
 *   //
 *   // Atomics:
 *   //   atomic<int32_t> g_counter; // fetch_add (seq_cst) @ 0x4013a0
 */
class ConcurrencyEmitter {
public:
    std::string emit(const ConcurrencyModel& model) const;
};

// ─── ConcurrencyDetector (orchestrator) ──────────────────────────────────────

/**
 * @brief Runs all sub-detectors over every function in an SSA module.
 */
class ConcurrencyDetector {
public:
    ConcurrencyDetector();

    /**
     * @brief Analyse one function.
     */
    void analyseFunction(const retdec::ssa::SSAFunction& fn,
                         ConcurrencyModel& out);

    /**
     * @brief Analyse all functions in a module (convenience wrapper).
     */
    ConcurrencyModel analyseModule(const retdec::ssa::SSAModule& mod);

private:
    std::vector<std::unique_ptr<IConcurrencyDetector>> detectors_;
};

} // namespace retdec::concurrency_detect

#endif // RETDEC_CONCURRENCY_DETECT_H
