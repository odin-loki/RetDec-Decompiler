/**
 * @file src/concurrency_detect/concurrency_detect.cpp
 * @brief Concurrency and Synchronisation Detector implementation.
 *
 * Each detector scans the function's SSA for:
 *   a) Direct call instructions whose target matches a known API symbol.
 *   b) IR-level primitives represented as Call instructions to compiler
 *      builtins (__atomic_*, __sync_*, cmpxchg variants).
 *   c) Structural patterns (tight CAS loop → spinlock, DCLP guard).
 */

#include <memory>
#include "retdec/concurrency_detect/concurrency_detect.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── Internal bridge types ─────────────────────────────────────────────────────
// These are file-private helpers that present a uniform view of call sites and
// atomic primitives extracted from the real retdec::ssa IR.

namespace {

struct CallEntry {
    std::string target;      ///< resolved callee name (demangled if possible)
    uint64_t    address = 0; ///< instruction VMA
};

struct AtomicEntry {
    std::string op;          ///< "load","store","exchange","cmpxchg","add",…
    std::string order;       ///< "relaxed","acquire","release","acq_rel","seq_cst"
    uint64_t    address = 0;
    uint64_t    varAddr = 0;
    std::string varName;
};

/// Extract all direct-call entries from a real SSA basic block.
std::vector<CallEntry> extractCalls(const retdec::ssa::BasicBlock& blk) {
    std::vector<CallEntry> result;
    for (const retdec::ssa::IrInstr* instr : blk.instrs) {
        if (!instr) continue;
        if (instr->op == retdec::ssa::IrInstr::Op::Call &&
            !instr->calleeName.empty()) {
            result.push_back({instr->calleeName, instr->vma});
        }
    }
    return result;
}

/// Derive atomic operation semantics from a compiler-builtin call name.
/// Returns true and fills 'ae' if the name matches an atomic intrinsic.
bool tryParseAtomicBuiltin(const std::string& name, uint64_t vma,
                            AtomicEntry& ae) {
    ae.address = vma;

    // GCC/Clang __atomic_* builtins
    if (name.find("__atomic_load")             != std::string::npos) { ae.op = "load";            }
    else if (name.find("__atomic_store")        != std::string::npos) { ae.op = "store";           }
    else if (name.find("__atomic_exchange")     != std::string::npos) { ae.op = "exchange";        }
    else if (name.find("__atomic_compare_exchange") != std::string::npos) { ae.op = "cmpxchg";    }
    else if (name.find("__atomic_fetch_add")    != std::string::npos) { ae.op = "add";             }
    else if (name.find("__atomic_fetch_sub")    != std::string::npos) { ae.op = "sub";             }
    else if (name.find("__atomic_fetch_and")    != std::string::npos) { ae.op = "and";             }
    else if (name.find("__atomic_fetch_or")     != std::string::npos) { ae.op = "or";              }
    else if (name.find("__atomic_fetch_xor")    != std::string::npos) { ae.op = "xor";             }
    else if (name.find("__atomic_fetch_nand")   != std::string::npos) { ae.op = "nand";            }
    // Legacy GCC __sync_* builtins
    else if (name.find("__sync_fetch_and_add")  != std::string::npos) { ae.op = "add";             }
    else if (name.find("__sync_fetch_and_sub")  != std::string::npos) { ae.op = "sub";             }
    else if (name.find("__sync_bool_compare_and_swap") != std::string::npos) { ae.op = "cmpxchg"; }
    else if (name.find("__sync_val_compare_and_swap")  != std::string::npos) { ae.op = "cmpxchg"; }
    else if (name.find("__sync_lock_test_and_set")     != std::string::npos) { ae.op = "exchange"; }
    else if (name.find("__sync_lock_release")          != std::string::npos) { ae.op = "store";    }
    else { return false; }

    // Memory ordering heuristic: __atomic_*_explicit names encode the ordering
    if (name.find("_relaxed") != std::string::npos)       ae.order = "relaxed";
    else if (name.find("_acquire") != std::string::npos)  ae.order = "acquire";
    else if (name.find("_release") != std::string::npos)  ae.order = "release";
    else if (name.find("_acq_rel") != std::string::npos)  ae.order = "acq_rel";
    else if (name.find("_seq_cst") != std::string::npos)  ae.order = "seq_cst";
    else                                                   ae.order = "seq_cst"; // conservative

    return true;
}

/// Extract atomic operations from a real SSA basic block.
std::vector<AtomicEntry> extractAtomics(const retdec::ssa::BasicBlock& blk) {
    std::vector<AtomicEntry> result;
    for (const retdec::ssa::IrInstr* instr : blk.instrs) {
        if (!instr || instr->op != retdec::ssa::IrInstr::Op::Call) continue;
        AtomicEntry ae;
        if (tryParseAtomicBuiltin(instr->calleeName, instr->vma, ae))
            result.push_back(ae);
    }
    return result;
}

/// Returns true if the block contains a CAS-like call (spinlock pattern).
bool hasCasLoop(const retdec::ssa::BasicBlock& blk) {
    for (const retdec::ssa::IrInstr* instr : blk.instrs) {
        if (!instr || instr->op != retdec::ssa::IrInstr::Op::Call) continue;
        const auto& n = instr->calleeName;
        if (n.find("compare_exchange")              != std::string::npos ||
            n.find("__atomic_compare_exchange")      != std::string::npos ||
            n.find("__sync_bool_compare_and_swap")   != std::string::npos ||
            n.find("__sync_val_compare_and_swap")    != std::string::npos ||
            n.find("InterlockedCompareExchange")      != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Returns true if the block matches the double-checked locking pattern
/// (atomic load followed by a mutex lock call in the same block).
bool isDCLP(const retdec::ssa::BasicBlock& blk) {
    bool hasAtomicLoad  = false;
    bool hasMutexLock   = false;
    for (const retdec::ssa::IrInstr* instr : blk.instrs) {
        if (!instr || instr->op != retdec::ssa::IrInstr::Op::Call) continue;
        const auto& n = instr->calleeName;
        if (!hasAtomicLoad &&
            (n.find("__atomic_load") != std::string::npos ||
             n.find("atomic_load")   != std::string::npos))
            hasAtomicLoad = true;
        if (!hasMutexLock &&
            (n.find("mutex") != std::string::npos ||
             n.find("lock")  != std::string::npos))
            hasMutexLock = true;
    }
    return hasAtomicLoad && hasMutexLock;
}

} // anonymous namespace

namespace retdec::concurrency_detect {

// ── Helper: case-insensitive contains ────────────────────────────────────────

static bool ciContains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

// ── Symbol tables ─────────────────────────────────────────────────────────────

// std::thread symbols
static const std::unordered_set<std::string> kStdThreadCreate = {
    "std::thread::thread", "_ZNSt6thread",
    "std::jthread::jthread",
};
static const std::unordered_set<std::string> kStdThreadJoin = {
    "std::thread::join", "_ZNSt6thread4joinEv",
    "std::thread::detach", "_ZNSt6thread6detachEv",
};
static const std::unordered_set<std::string> kStdMutexLock = {
    "std::mutex::lock", "_ZNSt5mutex4lockEv",
    "std::unique_lock::lock",
    "std::lock_guard::lock_guard",
    "std::scoped_lock::scoped_lock",
};
static const std::unordered_set<std::string> kStdMutexUnlock = {
    "std::mutex::unlock", "_ZNSt5mutex6unlockEv",
    "std::unique_lock::unlock",
    "std::lock_guard::~lock_guard",
    "std::scoped_lock::~scoped_lock",
};
static const std::unordered_set<std::string> kStdCondVar = {
    "std::condition_variable::wait",
    "std::condition_variable::wait_for",
    "std::condition_variable::wait_until",
    "std::condition_variable::notify_one",
    "std::condition_variable::notify_all",
};

// pthread symbols
static const std::unordered_set<std::string> kPthreadCreate = {
    "pthread_create",
};
static const std::unordered_set<std::string> kPthreadJoin = {
    "pthread_join", "pthread_detach",
};
static const std::unordered_set<std::string> kPthreadMutexLock = {
    "pthread_mutex_lock", "pthread_mutex_trylock",
    "pthread_rwlock_rdlock", "pthread_rwlock_wrlock",
    "pthread_rwlock_tryrdlock", "pthread_rwlock_trywrlock",
};
static const std::unordered_set<std::string> kPthreadMutexUnlock = {
    "pthread_mutex_unlock", "pthread_rwlock_unlock",
};
static const std::unordered_set<std::string> kPthreadCondVar = {
    "pthread_cond_wait", "pthread_cond_timedwait",
    "pthread_cond_signal", "pthread_cond_broadcast",
};
static const std::unordered_set<std::string> kPthreadSem = {
    "sem_init", "sem_wait", "sem_trywait", "sem_timedwait",
    "sem_post", "sem_destroy",
};
static const std::unordered_set<std::string> kPthreadBarrier = {
    "pthread_barrier_wait", "pthread_barrier_init",
};

// Win32 synchronisation symbols
static const std::unordered_set<std::string> kWin32ThreadCreate = {
    "CreateThread", "CreateRemoteThread", "_beginthreadex", "_beginthread",
};
static const std::unordered_set<std::string> kWin32ThreadWait = {
    "WaitForSingleObject", "WaitForMultipleObjects",
    "WaitForSingleObjectEx", "WaitForMultipleObjectsEx",
};
static const std::unordered_set<std::string> kWin32CS = {
    "InitializeCriticalSection", "InitializeCriticalSectionEx",
    "EnterCriticalSection", "TryEnterCriticalSection",
    "LeaveCriticalSection", "DeleteCriticalSection",
};
static const std::unordered_set<std::string> kWin32SRW = {
    "AcquireSRWLockExclusive", "AcquireSRWLockShared",
    "ReleaseSRWLockExclusive", "ReleaseSRWLockShared",
    "TryAcquireSRWLockExclusive", "TryAcquireSRWLockShared",
    "InitializeSRWLock",
};
static const std::unordered_set<std::string> kWin32CondVar = {
    "InitializeConditionVariable",
    "SleepConditionVariableCS", "SleepConditionVariableSRW",
    "WakeConditionVariable", "WakeAllConditionVariable",
};
static const std::unordered_set<std::string> kWin32Sem = {
    "CreateSemaphore", "CreateSemaphoreEx",
    "ReleaseSemaphore", "OpenSemaphore",
};
static const std::unordered_set<std::string> kWin32Event = {
    "CreateEvent", "CreateEventEx", "SetEvent", "ResetEvent", "PulseEvent",
};
static const std::unordered_set<std::string> kWin32Interlocked = {
    "InterlockedIncrement", "InterlockedDecrement",
    "InterlockedExchange", "InterlockedExchangeAdd",
    "InterlockedCompareExchange", "InterlockedAnd", "InterlockedOr",
    "InterlockedXor", "InterlockedAdd",
    "InterlockedIncrement64", "InterlockedDecrement64",
    "InterlockedExchange64", "InterlockedCompareExchange64",
};
static const std::unordered_set<std::string> kWin32Once = {
    "InitOnceExecuteOnce", "InitOnceBeginInitialize", "InitOnceComplete",
};

// OpenMP symbols
static const std::unordered_set<std::string> kOmpFork = {
    "GOMP_parallel", "GOMP_parallel_start", "GOMP_parallel_end",
    "__kmpc_fork_call", "__kmpc_fork_teams",
};
static const std::unordered_set<std::string> kOmpBarrier = {
    "GOMP_barrier", "__kmpc_barrier",
};
static const std::unordered_set<std::string> kOmpCritical = {
    "GOMP_critical_start", "GOMP_critical_end",
    "__kmpc_critical", "__kmpc_end_critical",
};
static const std::unordered_set<std::string> kOmpAtomic = {
    "GOMP_atomic_start", "GOMP_atomic_end",
    "__kmpc_atomic_fixed4_add", "__kmpc_atomic_float4_add",
};

// TBB symbols
static const std::unordered_set<std::string> kTBBPatterns = {
    "tbb::parallel_for", "tbb::parallel_reduce", "tbb::parallel_invoke",
    "tbb::task_group::run", "tbb::task_group::wait",
    "tbb::queuing_mutex::scoped_lock",
    "tbb::spin_mutex::scoped_lock",
    "tbb::reader_writer_lock::scoped_lock",
    "tbb::concurrent_queue", "tbb::concurrent_vector",
    "tbb::enumerable_thread_specific",
};

// ── Symbol lookup helpers ─────────────────────────────────────────────────────

static bool matchesAny(const std::string& sym,
                        const std::unordered_set<std::string>& set) {
    if (set.count(sym)) return true;
    // Also try prefix match for mangled names
    for (const auto& s : set) {
        if (sym.find(s) != std::string::npos) return true;
    }
    return false;
}

// Convert string memory ordering to enum
static AtomicOrder parseOrder(const std::string& s) {
    if (s == "relaxed")  return AtomicOrder::Relaxed;
    if (s == "consume")  return AtomicOrder::Consume;
    if (s == "acquire")  return AtomicOrder::Acquire;
    if (s == "release")  return AtomicOrder::Release;
    if (s == "acq_rel")  return AtomicOrder::AcqRel;
    if (s == "seq_cst")  return AtomicOrder::SeqCst;
    return AtomicOrder::Unknown;
}

static AtomicOp parseOp(const std::string& s) {
    if (s == "load")             return AtomicOp::Load;
    if (s == "store")            return AtomicOp::Store;
    if (s == "atomicrmw" || s == "xchg" || s == "exchange")
                                 return AtomicOp::Exchange;
    if (s == "cmpxchg" || s == "cmpxchg_weak")
                                 return AtomicOp::CompareExchange;
    if (s == "add"  || s == "fetch_add")  return AtomicOp::FetchAdd;
    if (s == "sub"  || s == "fetch_sub")  return AtomicOp::FetchSub;
    if (s == "and"  || s == "fetch_and")  return AtomicOp::FetchAnd;
    if (s == "or"   || s == "fetch_or" )  return AtomicOp::FetchOr;
    if (s == "xor"  || s == "fetch_xor")  return AtomicOp::FetchXor;
    if (s == "nand" || s == "fetch_nand") return AtomicOp::FetchNand;
    if (s == "fence")            return AtomicOp::Fence;
    return AtomicOp::Unknown;
}

// ─── ConcurrencyModel::merge ──────────────────────────────────────────────────

void ConcurrencyModel::merge(const ConcurrencyModel& other) {
    threads.insert   (threads.end(),    other.threads.begin(),    other.threads.end());
    locks.insert     (locks.end(),      other.locks.begin(),      other.locks.end());
    atomics.insert   (atomics.end(),    other.atomics.begin(),    other.atomics.end());
    condVars.insert  (condVars.end(),   other.condVars.begin(),   other.condVars.end());
    semaphores.insert(semaphores.end(), other.semaphores.begin(), other.semaphores.end());
    barriers.insert  (barriers.end(),   other.barriers.begin(),   other.barriers.end());
    spinlocks.insert (spinlocks.end(),  other.spinlocks.begin(),  other.spinlocks.end());
    ompRegions.insert(ompRegions.end(), other.ompRegions.begin(), other.ompRegions.end());
    tbbPatterns.insert(tbbPatterns.end(),other.tbbPatterns.begin(),other.tbbPatterns.end());
    if (!isMT) isMT = other.isMT;
    if (primaryLib == ThreadLib::Unknown) primaryLib = other.primaryLib;
}

// ─── StdThreadDetector ────────────────────────────────────────────────────────

void StdThreadDetector::analyseFunction(const ssa::SSAFunction& fn,
                                         ConcurrencyModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractCalls(*blkPtr)) {
            const auto& sym = call.target;

            if (matchesAny(sym, kStdThreadCreate)) {
                ThreadInfo ti;
                ti.lib      = ThreadLib::StdThread;
                ti.funcName = fnName;
                ti.callSite = call.address;
                out.threads.push_back(ti);
                out.isMT = true;
                if (out.primaryLib == ThreadLib::Unknown)
                    out.primaryLib = ThreadLib::StdThread;
            }
            if (matchesAny(sym, kStdThreadJoin)) {
                if (!out.threads.empty()) {
                    out.threads.back().isJoined  = sym.find("join")   != std::string::npos;
                    out.threads.back().isDetached = sym.find("detach") != std::string::npos;
                }
            }
            if (matchesAny(sym, kStdMutexLock)) {
                LockInfo li;
                li.kind         = MutexKind::StdMutex;
                li.funcName     = fnName;
                li.lockCall     = call.address;
                li.isLockGuard  = (sym.find("lock_guard") != std::string::npos ||
                                   sym.find("scoped_lock") != std::string::npos ||
                                   sym.find("unique_lock") != std::string::npos);
                out.locks.push_back(li);
            }
            if (matchesAny(sym, kStdMutexUnlock)) {
                if (!out.locks.empty())
                    out.locks.back().unlockCall = call.address;
            }
            if (matchesAny(sym, kStdCondVar)) {
                CondVarInfo cv;
                cv.lib      = CondVarLib::StdCondVar;
                cv.funcName = fnName;
                if (sym.find("wait") != std::string::npos)
                    cv.waitCall = call.address;
                else {
                    cv.notifyCall = call.address;
                    cv.notifyAll  = (sym.find("all") != std::string::npos);
                }
                out.condVars.push_back(cv);
            }
        }
    }
}

// ─── PthreadDetector ──────────────────────────────────────────────────────────

void PthreadDetector::analyseFunction(const ssa::SSAFunction& fn,
                                       ConcurrencyModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractCalls(*blkPtr)) {
            const auto& sym = call.target;

            if (matchesAny(sym, kPthreadCreate)) {
                ThreadInfo ti;
                ti.lib      = ThreadLib::PThread;
                ti.funcName = fnName;
                ti.callSite = call.address;
                out.threads.push_back(ti);
                out.isMT = true;
                if (out.primaryLib == ThreadLib::Unknown)
                    out.primaryLib = ThreadLib::PThread;
            }
            if (matchesAny(sym, kPthreadJoin)) {
                if (!out.threads.empty()) {
                    out.threads.back().isJoined  = (sym == "pthread_join");
                    out.threads.back().isDetached = (sym == "pthread_detach");
                }
            }
            if (matchesAny(sym, kPthreadMutexLock)) {
                LockInfo li;
                li.kind     = ciContains(sym, "rwlock") ?
                              MutexKind::PthreadRwLock : MutexKind::PthreadMutex;
                li.funcName = fnName;
                li.lockCall = call.address;
                li.isTryLock = (sym.find("try") != std::string::npos);
                out.locks.push_back(li);
            }
            if (matchesAny(sym, kPthreadMutexUnlock)) {
                if (!out.locks.empty())
                    out.locks.back().unlockCall = call.address;
            }
            if (matchesAny(sym, kPthreadCondVar)) {
                CondVarInfo cv;
                cv.lib      = CondVarLib::Pthread;
                cv.funcName = fnName;
                if (sym.find("wait") != std::string::npos)
                    cv.waitCall = call.address;
                else {
                    cv.notifyCall = call.address;
                    cv.notifyAll  = (sym == "pthread_cond_broadcast");
                }
                out.condVars.push_back(cv);
            }
            if (matchesAny(sym, kPthreadSem)) {
                SemaphoreInfo si;
                si.kind     = SemKind::PosixSem;
                si.funcName = fnName;
                if (sym.find("wait") != std::string::npos || sym == "sem_wait")
                    si.waitCall = call.address;
                else if (sym == "sem_post")
                    si.postCall = call.address;
                out.semaphores.push_back(si);
            }
            if (matchesAny(sym, kPthreadBarrier)) {
                BarrierInfo bi;
                bi.lib      = ThreadLib::PThread;
                bi.funcName = fnName;
                bi.callAddr = call.address;
                out.barriers.push_back(bi);
            }
        }
    }
}

// ─── Win32ThreadDetector ──────────────────────────────────────────────────────

void Win32ThreadDetector::analyseFunction(const ssa::SSAFunction& fn,
                                           ConcurrencyModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractCalls(*blkPtr)) {
            const auto& sym = call.target;

            if (matchesAny(sym, kWin32ThreadCreate)) {
                ThreadInfo ti;
                ti.lib      = ThreadLib::Win32;
                ti.funcName = fnName;
                ti.callSite = call.address;
                out.threads.push_back(ti);
                out.isMT = true;
                if (out.primaryLib == ThreadLib::Unknown)
                    out.primaryLib = ThreadLib::Win32;
            }
            if (matchesAny(sym, kWin32CS)) {
                if (sym == "EnterCriticalSection" || sym == "TryEnterCriticalSection") {
                    LockInfo li;
                    li.kind      = MutexKind::Win32CriticalSection;
                    li.funcName  = fnName;
                    li.lockCall  = call.address;
                    li.isTryLock = (sym == "TryEnterCriticalSection");
                    out.locks.push_back(li);
                } else if (sym == "LeaveCriticalSection") {
                    if (!out.locks.empty())
                        out.locks.back().unlockCall = call.address;
                }
            }
            if (matchesAny(sym, kWin32SRW)) {
                if (ciContains(sym, "Acquire")) {
                    LockInfo li;
                    li.kind     = MutexKind::Win32SRWLock;
                    li.funcName = fnName;
                    li.lockCall = call.address;
                    out.locks.push_back(li);
                } else if (ciContains(sym, "Release")) {
                    if (!out.locks.empty())
                        out.locks.back().unlockCall = call.address;
                }
            }
            if (matchesAny(sym, kWin32CondVar)) {
                CondVarInfo cv;
                cv.lib      = CondVarLib::Win32CondVar;
                cv.funcName = fnName;
                if (ciContains(sym, "Sleep"))
                    cv.waitCall = call.address;
                else {
                    cv.notifyCall = call.address;
                    cv.notifyAll  = ciContains(sym, "All");
                }
                out.condVars.push_back(cv);
            }
            if (matchesAny(sym, kWin32Sem)) {
                SemaphoreInfo si;
                si.kind     = SemKind::Win32Sem;
                si.funcName = fnName;
                if (sym == "ReleaseSemaphore")
                    si.postCall = call.address;
                else
                    si.waitCall = call.address;
                out.semaphores.push_back(si);
            }
            if (matchesAny(sym, kWin32Interlocked)) {
                AtomicInfo ai;
                ai.funcName = fnName;
                ai.address  = call.address;
                ai.order    = AtomicOrder::SeqCst;  // Interlocked is always seq_cst
                if (sym.find("Increment") != std::string::npos)
                    ai.op = AtomicOp::FetchAdd;
                else if (sym.find("Decrement") != std::string::npos)
                    ai.op = AtomicOp::FetchSub;
                else if (sym.find("CompareExchange") != std::string::npos)
                    ai.op = AtomicOp::CompareExchange;
                else if (sym.find("Exchange") != std::string::npos)
                    ai.op = AtomicOp::Exchange;
                else
                    ai.op = AtomicOp::FetchAdd;
                out.atomics.push_back(ai);
            }
        }
    }
}

// ─── AtomicDetector ───────────────────────────────────────────────────────────

void AtomicDetector::analyseFunction(const ssa::SSAFunction& fn,
                                      ConcurrencyModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        // Detect atomic builtins encoded as Call instructions
        for (const auto& ae : extractAtomics(*blkPtr)) {
            AtomicInfo info;
            info.funcName = fnName;
            info.address  = ae.address;
            info.varAddr  = ae.varAddr;
            info.varName  = ae.varName;
            info.op       = parseOp(ae.op);
            info.order    = parseOrder(ae.order);
            out.atomics.push_back(info);
            if (!out.isMT && info.op != AtomicOp::Unknown)
                out.isMT = true;
        }
    }
}

// ─── SpinlockDetector ─────────────────────────────────────────────────────────

void SpinlockDetector::analyseFunction(const ssa::SSAFunction& fn,
                                        ConcurrencyModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        if (hasCasLoop(*blkPtr)) {
            SpinlockInfo si;
            si.funcName = fnName;
            si.loopAddr = blkPtr->id != ssa::kInvalidBlock ? blkPtr->id : 0;
            si.isDCLP   = isDCLP(*blkPtr);
            out.spinlocks.push_back(si);
            out.isMT = true;
        }
    }
}

// ─── OpenMPDetector ───────────────────────────────────────────────────────────

void OpenMPDetector::analyseFunction(const ssa::SSAFunction& fn,
                                      ConcurrencyModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractCalls(*blkPtr)) {
            const auto& sym = call.target;
            if (matchesAny(sym, kOmpFork)) {
                OpenMPRegion reg;
                reg.kind     = "parallel";
                reg.funcName = fnName;
                reg.forkCall = call.address;
                out.ompRegions.push_back(reg);
                out.isMT = true;
                if (out.primaryLib == ThreadLib::Unknown)
                    out.primaryLib = ThreadLib::OpenMP;
            }
            if (matchesAny(sym, kOmpBarrier)) {
                BarrierInfo bi;
                bi.lib      = ThreadLib::OpenMP;
                bi.funcName = fnName;
                bi.callAddr = call.address;
                out.barriers.push_back(bi);
                OpenMPRegion reg;
                reg.kind     = "barrier";
                reg.funcName = fnName;
                reg.forkCall = call.address;
                out.ompRegions.push_back(reg);
            }
            if (matchesAny(sym, kOmpCritical)) {
                OpenMPRegion reg;
                reg.kind     = "critical";
                reg.funcName = fnName;
                reg.forkCall = call.address;
                out.ompRegions.push_back(reg);
            }
            if (matchesAny(sym, kOmpAtomic)) {
                OpenMPRegion reg;
                reg.kind     = "atomic";
                reg.funcName = fnName;
                reg.forkCall = call.address;
                out.ompRegions.push_back(reg);
            }
        }
    }
}

// ─── TBBDetector ─────────────────────────────────────────────────────────────

void TBBDetector::analyseFunction(const ssa::SSAFunction& fn,
                                   ConcurrencyModel& out) {
    const std::string& fnName = fn.name();
    for (const auto& blkPtr : fn.blocks()) {
        for (const auto& call : extractCalls(*blkPtr)) {
            for (const auto& pattern : kTBBPatterns) {
                if (call.target.find(pattern) != std::string::npos) {
                    TBBPattern tp;
                    tp.funcName = fnName;
                    tp.callAddr = call.address;
                    // Derive kind from symbol
                    if (pattern.find("parallel_for") != std::string::npos)
                        tp.kind = "parallel_for";
                    else if (pattern.find("task_group") != std::string::npos)
                        tp.kind = "task_group";
                    else if (pattern.find("mutex") != std::string::npos ||
                             pattern.find("lock")  != std::string::npos)
                        tp.kind = "mutex";
                    else if (pattern.find("queue") != std::string::npos)
                        tp.kind = "concurrent_queue";
                    else if (pattern.find("vector") != std::string::npos)
                        tp.kind = "concurrent_vector";
                    else
                        tp.kind = "tbb";
                    out.tbbPatterns.push_back(tp);
                    out.isMT = true;
                    if (out.primaryLib == ThreadLib::Unknown)
                        out.primaryLib = ThreadLib::TBB;
                    break;
                }
            }
        }
    }
}

// ─── ConcurrencyEmitter ───────────────────────────────────────────────────────

static const char* threadLibName(ThreadLib lib) {
    switch (lib) {
    case ThreadLib::StdThread: return "std::thread (C++11)";
    case ThreadLib::PThread:   return "pthreads";
    case ThreadLib::Win32:     return "Win32 threads";
    case ThreadLib::OpenMP:    return "OpenMP";
    case ThreadLib::TBB:       return "Intel TBB";
    case ThreadLib::Custom:    return "custom thread pool";
    default:                   return "unknown";
    }
}

static const char* mutexKindName(MutexKind k) {
    switch (k) {
    case MutexKind::StdMutex:             return "std::mutex";
    case MutexKind::StdRecursiveMutex:    return "std::recursive_mutex";
    case MutexKind::StdSharedMutex:       return "std::shared_mutex";
    case MutexKind::PthreadMutex:         return "pthread_mutex_t";
    case MutexKind::PthreadRwLock:        return "pthread_rwlock_t";
    case MutexKind::Win32CriticalSection: return "CRITICAL_SECTION";
    case MutexKind::Win32Mutex:           return "HANDLE (Mutex)";
    case MutexKind::Win32SRWLock:         return "SRWLOCK";
    case MutexKind::OmpCritical:          return "OMP critical";
    case MutexKind::TBBMutex:             return "tbb::mutex";
    case MutexKind::Spinlock:             return "spinlock";
    default:                              return "mutex";
    }
}

static const char* atomicOpName(AtomicOp op) {
    switch (op) {
    case AtomicOp::Load:            return "load";
    case AtomicOp::Store:           return "store";
    case AtomicOp::Exchange:        return "exchange";
    case AtomicOp::CompareExchange: return "compare_exchange";
    case AtomicOp::FetchAdd:        return "fetch_add";
    case AtomicOp::FetchSub:        return "fetch_sub";
    case AtomicOp::FetchAnd:        return "fetch_and";
    case AtomicOp::FetchOr:         return "fetch_or";
    case AtomicOp::FetchXor:        return "fetch_xor";
    case AtomicOp::FetchNand:       return "fetch_nand";
    case AtomicOp::Fence:           return "fence";
    default:                        return "atomic_op";
    }
}

static const char* atomicOrderName(AtomicOrder o) {
    switch (o) {
    case AtomicOrder::Relaxed: return "memory_order_relaxed";
    case AtomicOrder::Consume: return "memory_order_consume";
    case AtomicOrder::Acquire: return "memory_order_acquire";
    case AtomicOrder::Release: return "memory_order_release";
    case AtomicOrder::AcqRel:  return "memory_order_acq_rel";
    case AtomicOrder::SeqCst:  return "memory_order_seq_cst";
    default:                   return "memory_order_unknown";
    }
}

std::string ConcurrencyEmitter::emit(const ConcurrencyModel& model) const {
    std::ostringstream os;
    os << "// === Concurrency Model ===\n";
    os << "// Multithreaded: " << (model.isMT ? "yes" : "no") << "\n";
    os << "// Primary threading library: " << threadLibName(model.primaryLib) << "\n\n";

    if (!model.threads.empty()) {
        os << "// Threads (" << model.threads.size() << "):\n";
        for (const auto& t : model.threads) {
            os << "//   [" << threadLibName(t.lib) << "] "
               << t.funcName << " @ 0x" << std::hex << t.callSite << std::dec;
            if (!t.threadFunc.empty()) os << " → " << t.threadFunc;
            if (t.isJoined)   os << " (joined)";
            if (t.isDetached) os << " (detached)";
            os << "\n";
        }
        os << "\n";
    }

    if (!model.locks.empty()) {
        os << "// Locks (" << model.locks.size() << "):\n";
        for (const auto& l : model.locks) {
            os << "//   " << mutexKindName(l.kind) << " in " << l.funcName;
            if (l.lockCall)   os << "  lock@0x"   << std::hex << l.lockCall;
            if (l.unlockCall) os << "  unlock@0x" << std::hex << l.unlockCall;
            os << std::dec;
            if (l.isLockGuard) os << " [RAII guard]";
            if (l.isTryLock)   os << " [try_lock]";
            os << "\n";
        }
        os << "\n";
    }

    if (!model.atomics.empty()) {
        os << "// Atomic operations (" << model.atomics.size() << "):\n";
        for (const auto& a : model.atomics) {
            os << "//   " << atomicOpName(a.op)
               << " (" << atomicOrderName(a.order) << ")"
               << " @ 0x" << std::hex << a.address << std::dec
               << " in " << a.funcName;
            if (!a.varName.empty()) os << "  var=" << a.varName;
            os << "\n";
        }
        os << "\n";
    }

    if (!model.condVars.empty()) {
        os << "// Condition variables (" << model.condVars.size() << "):\n";
        for (const auto& cv : model.condVars) {
            os << "//   in " << cv.funcName;
            if (cv.waitCall)   os << "  wait@0x"  << std::hex << cv.waitCall;
            if (cv.notifyCall) os << "  notify@0x" << std::hex << cv.notifyCall;
            os << std::dec;
            if (cv.notifyAll) os << " (notify_all)";
            os << "\n";
        }
        os << "\n";
    }

    if (!model.semaphores.empty()) {
        os << "// Semaphores (" << model.semaphores.size() << "):\n";
        for (const auto& s : model.semaphores) {
            os << "//   " << (s.kind == SemKind::PosixSem ? "sem_t" : "HANDLE(Sem)")
               << " in " << s.funcName;
            if (s.waitCall) os << "  wait@0x" << std::hex << s.waitCall;
            if (s.postCall) os << "  post@0x" << std::hex << s.postCall;
            os << std::dec;
            os << "\n";
        }
        os << "\n";
    }

    if (!model.barriers.empty()) {
        os << "// Barriers (" << model.barriers.size() << "):\n";
        for (const auto& b : model.barriers) {
            os << "//   [" << threadLibName(b.lib) << "] "
               << b.funcName << " @ 0x" << std::hex << b.callAddr << std::dec << "\n";
        }
        os << "\n";
    }

    if (!model.spinlocks.empty()) {
        os << "// Spinlocks (" << model.spinlocks.size() << "):\n";
        for (const auto& s : model.spinlocks) {
            os << "//   in " << s.funcName;
            if (s.loopAddr) os << " @ 0x" << std::hex << s.loopAddr << std::dec;
            if (s.isDCLP)   os << " [DCLP pattern]";
            os << "\n";
        }
        os << "\n";
    }

    if (!model.ompRegions.empty()) {
        os << "// OpenMP regions (" << model.ompRegions.size() << "):\n";
        for (const auto& r : model.ompRegions) {
            os << "//   #pragma omp " << r.kind
               << " in " << r.funcName
               << " @ 0x" << std::hex << r.forkCall << std::dec << "\n";
        }
        os << "\n";
    }

    if (!model.tbbPatterns.empty()) {
        os << "// Intel TBB patterns (" << model.tbbPatterns.size() << "):\n";
        for (const auto& t : model.tbbPatterns) {
            os << "//   tbb::" << t.kind
               << " in " << t.funcName
               << " @ 0x" << std::hex << t.callAddr << std::dec << "\n";
        }
        os << "\n";
    }

    return os.str();
}

// ─── ConcurrencyDetector (orchestrator) ──────────────────────────────────────

ConcurrencyDetector::ConcurrencyDetector() {
    detectors_.push_back(std::make_unique<StdThreadDetector>());
    detectors_.push_back(std::make_unique<PthreadDetector>());
    detectors_.push_back(std::make_unique<Win32ThreadDetector>());
    detectors_.push_back(std::make_unique<AtomicDetector>());
    detectors_.push_back(std::make_unique<SpinlockDetector>());
    detectors_.push_back(std::make_unique<OpenMPDetector>());
    detectors_.push_back(std::make_unique<TBBDetector>());
}

void ConcurrencyDetector::analyseFunction(const ssa::SSAFunction& fn,
                                           ConcurrencyModel& out) {
    for (auto& d : detectors_)
        d->analyseFunction(fn, out);
}

ConcurrencyModel ConcurrencyDetector::analyseModule(const ssa::SSAModule& mod) {
    ConcurrencyModel result;
    for (const auto& fnPtr : mod.functions)
        if (fnPtr) analyseFunction(*fnPtr, result);
    return result;
}

} // namespace retdec::concurrency_detect
