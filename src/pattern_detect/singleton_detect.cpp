/**
 * @file src/pattern_detect/singleton_detect.cpp
 * @brief Singleton design pattern detector.
 *
 * ## Structural invariant
 *
 * A compiled Singleton's getInstance() function always has:
 *   1. A Load of a static (global) pointer value.
 *   2. A Compare of that pointer against zero (null-check).
 *   3. A conditional Branch: the taken (null) path leads to:
 *        a. A Call to malloc / operator new.
 *        b. A Store of the result to the static pointer.
 *   4. A Ret instruction returning the pointer.
 *
 * ## Double-checked-lock variant
 *
 * Thread-safe Singleton adds two null-checks separated by a lock acquire:
 *   ```
 *   if (!instance) {
 *       lock_acquire();
 *       if (!instance)          ← second null-check inside lock
 *           instance = new T();
 *       lock_release();
 *   }
 *   ```
 *
 * IR signal: two Compare-against-zero instructions in the function, with a
 * Call to a lock-acquire function (pthread_mutex_lock, EnterCriticalSection,
 * __cxa_guard_acquire, etc.) between them.
 *
 * ## Confidence scoring
 *
 *   static pointer Load         +0.25
 *   null-check Compare          +0.25
 *   first-access allocation     +0.30
 *   return pointer              +0.20
 *   double-checked-lock bonus   +0.10 (additive, capped at 1.0)
 */

#include "retdec/pattern_detect/pattern_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace pattern_detect {

namespace {

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op) {
    int n = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs)
            if (i && i->op == op) ++n;
    }
    return n;
}

// Null-check: Compare with Immediate 0.
static bool hasNullCheck(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Compare) continue;
            for (const auto& u : i->uses) {
                const auto* v = fn.value(u.valueId);
                if (v && v->kind == ssa::ValueKind::Immediate && v->imm == 0)
                    return true;
            }
        }
    }
    return false;
}

// First-access allocation: a Call to malloc/new + a Store after the null branch.
static bool hasFirstAlloc(const ssa::SSAFunction& fn) {
    bool hasMalloc = false, hasStore = false;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i) continue;
            if (i->op == ssa::IrInstr::Op::Call) {
                const auto& cn = i->calleeName;
                if (cn == "malloc" || cn == "_Znwm" || cn == "operator new" ||
                    cn.find("allocate") != std::string::npos ||
                    cn.find("__cxa_guard") != std::string::npos)
                    hasMalloc = true;
            }
            if (i->op == ssa::IrInstr::Op::Store) hasStore = true;
        }
    }
    return hasMalloc && hasStore;
}

// Lock-acquire call: pthread_mutex_lock, EnterCriticalSection, __cxa_guard_acquire.
static bool hasLockAcquire(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = i->calleeName;
            if (cn.find("mutex_lock")          != std::string::npos ||
                cn.find("EnterCriticalSection") != std::string::npos ||
                cn.find("__cxa_guard_acquire")  != std::string::npos ||
                cn.find("pthread_mutex_lock")   != std::string::npos ||
                cn.find("AcquireSRWLock")       != std::string::npos)
                return true;
        }
    }
    return false;
}

} // anonymous namespace

bool SingletonDetector::hasDoubleLock(const ssa::SSAFunction& fn) const {
    int nullChecks = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Compare) continue;
            for (const auto& u : i->uses) {
                const auto* v = fn.value(u.valueId);
                if (v && v->kind == ssa::ValueKind::Immediate && v->imm == 0)
                    ++nullChecks;
            }
        }
    }
    return nullChecks >= 2 && hasLockAcquire(fn);
}

SingletonEvidence SingletonDetector::analyse(const ssa::SSAFunction& fn) const {
    SingletonEvidence ev;
    ev.hasStaticPtrLoad = countOp(fn, ssa::IrInstr::Op::Load)    >= 1;
    ev.hasNullCheck     = hasNullCheck(fn);
    ev.hasFirstAlloc    = hasFirstAlloc(fn);
    ev.hasReturn        = countOp(fn, ssa::IrInstr::Op::Ret)     >= 1;
    ev.hasDoubleLock    = hasDoubleLock(fn);
    ev.found = ev.hasNullCheck && ev.hasFirstAlloc;
    ev.confidence = score(ev);
    return ev;
}

float SingletonDetector::score(const SingletonEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasStaticPtrLoad) s += 0.25f;
    if (ev.hasNullCheck)     s += 0.25f;
    if (ev.hasFirstAlloc)    s += 0.30f;
    if (ev.hasReturn)        s += 0.20f;
    if (ev.hasDoubleLock)    s += 0.10f;
    return s > 1.0f ? 1.0f : s;
}

PatternResult SingletonDetector::detect(const ssa::SSAFunction& fn) const {
    PatternResult r;
    r.kind = PatternKind::Singleton;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.hasDoubleLock) {
        r.hasVariant  = true;
        r.variantName = "double-checked-lock";
    }
    if (ev.confidence >= 0.45f) {
        r.emittedForm =
            "static T* instance = nullptr;\n"
            "static T* getInstance() {\n"
            "    if (!instance) instance = new T();\n"
            "    return instance;\n"
            "}";
        r.comment = "// Design pattern: Singleton" +
            std::string(ev.hasDoubleLock ? " (double-checked locking)" : "");
    }
    return r;
}

PatternResult SingletonDetector::detectGroup(
        const std::vector<const ssa::SSAFunction*>& fns) const {
    PatternResult best;
    for (const auto* fn : fns) {
        if (!fn) continue;
        auto r = detect(*fn);
        if (r.confidence > best.confidence) best = r;
    }
    return best;
}

} // namespace pattern_detect
} // namespace retdec
