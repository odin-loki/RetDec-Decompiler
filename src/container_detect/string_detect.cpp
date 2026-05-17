/**
 * @file src/container_detect/string_detect.cpp
 * @brief std::string (SSO) and std::shared_ptr<T> detector.
 *
 * ## std::string — Small String Optimisation (SSO) fingerprint
 *
 * ### libstdc++ (GCC) layout (64-bit, threshold = 15 chars)
 *
 *   ```cpp
 *   union {
 *       char   _M_local_buf[_S_local_capacity + 1];  // 16 bytes inline
 *       char*  _M_allocated_capacity;
 *   };
 *   size_type _M_string_length;
 *   char*     _M_dataplus._M_p;  // points to _M_local_buf or heap
 *   ```
 *
 * ### libc++ (Clang) layout (64-bit, threshold = 22 chars)
 *
 *   ```cpp
 *   // Short mode: top bit of first byte == 0
 *   // Long mode:  top bit of first byte == 1
 *   struct { uint8_t size; char data[22]; };      // short
 *   struct { size_t  cap; size_t len; char* ptr; };// long
 *   ```
 *
 * ### MSVC STL layout (64-bit, threshold = 15 chars)
 *
 *   ```cpp
 *   union { char buf[16]; char* ptr; };
 *   size_type size;
 *   size_type cap;
 *   ```
 *
 * ### SSO branch signal (all variants)
 *
 *   A Compare instruction testing the string length against a small threshold
 *   constant (15, 22, or 23), followed by two different load/store paths:
 *   - Short path: access within the struct itself (base + small_offset).
 *   - Long path:  load the heap pointer, then access through it.
 *
 * ## std::shared_ptr<T> fingerprint
 *
 * Two-pointer layout:
 *   ```cpp
 *   T*              _M_ptr;        // offset 0
 *   _Sp_counted_base* _M_refcount; // offset 8 (or 4 on 32-bit)
 *   ```
 *
 * Control block (reference counting):
 *   ```cpp
 *   struct _Sp_counted_base {
 *       atomic<int> _M_use_count;   // strong count
 *       atomic<int> _M_weak_count;  // weak count
 *   };
 *   ```
 *
 * Signals:
 *   - Two Loads from adjacent pointer-width offsets.
 *   - An atomic decrement of one of the loaded values (Sub + CompareExchange
 *     or lock-prefixed decrement on x86, LDREX/STREX on ARM).
 *   - A zero-check Compare followed by a Call to `free` or `operator delete`.
 *   - An atomic increment on copy (for shared ownership).
 */

#include <memory>
#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <cstdint>

namespace retdec {
namespace container_detect {

namespace {

// ─── Helpers ─────────────────────────────────────────────────────────────────

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op) {
    int n = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs)
            if (instr && instr->op == op) ++n;
    }
    return n;
}

// Inline path: address arithmetic within the struct itself — small-constant Add.
static bool hasInlinePath(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Add) continue;
            for (const auto& use : instr->uses) {
                const auto* iv = fn.value(use.valueId);
                if (iv && iv->kind == ssa::ValueKind::Immediate && iv->imm < 32)
                    return true;
            }
        }
    }
    return false;
}

// Heap path: a Load of a pointer followed by a further Load (pointer-to-chars).
static bool hasHeapPath(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Load) >= 2;
}

static bool hasZeroCheckFree(const ssa::SSAFunction& fn) {
    bool hasFree = false, hasCompareZero = false;
    bool hasSub = false;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Sub) hasSub = true;
            if (instr->op == ssa::IrInstr::Op::Call) {
                const auto& cn = instr->calleeName;
                if (cn == "free" || cn == "_ZdlPv" || cn == "operator delete" ||
                    cn.find("deallocate") != std::string::npos)
                    hasFree = true;
            }
            if (instr->op == ssa::IrInstr::Op::Compare) {
                // Accept explicit comparison against 0.
                for (const auto& u : instr->uses) {
                    const auto* iv = fn.value(u.valueId);
                    if (iv && iv->kind == ssa::ValueKind::Immediate && iv->imm == 0) {
                        hasCompareZero = true;
                        break;
                    }
                }
                // Accept Sub+Compare pattern (atomic decrement then check).
                if (hasSub) hasCompareZero = true;
            }
        }
    }
    return hasFree && hasCompareZero;
}

} // anonymous namespace

bool StringDetector::hasSSOThresholdCompare(const ssa::SSAFunction& fn,
                                             int& threshold) const {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Compare) continue;
            for (const auto& use : instr->uses) {
                const auto* iv = fn.value(use.valueId);
                if (!iv || iv->kind != ssa::ValueKind::Immediate) continue;
                uint64_t c = iv->imm;
                if (c == 15 || c == 22 || c == 23) {
                    threshold = static_cast<int>(c);
                    return true;
                }
            }
        }
    }
    return false;
}

bool SharedPtrDetector::hasAtomicOperation(const ssa::SSAFunction& fn) const {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Call) {
                const auto& cn = instr->calleeName;
                if (cn.find("atomic")          != std::string::npos ||
                    cn.find("__atomic")        != std::string::npos ||
                    cn.find("_InterlockedDecrement") != std::string::npos ||
                    cn.find("_InterlockedIncrement") != std::string::npos)
                    return true;
            }
            if (instr->op == ssa::IrInstr::Op::Sub)
                return countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
        }
    }
    return false;
}

// ─── StringDetector ──────────────────────────────────────────────────────────

SSOEvidence StringDetector::analyseStructure(const ssa::SSAFunction& fn) const {
    SSOEvidence ev;
    int threshold = 0;
    ev.hasSSOBranch  = hasSSOThresholdCompare(fn, threshold);
    ev.ssoThreshold  = threshold;
    ev.hasInlinePath = hasInlinePath(fn);
    ev.hasHeapPath   = hasHeapPath(fn);
    ev.found = ev.hasSSOBranch;
    ev.confidence = scoreEvidence(ev);
    return ev;
}

float StringDetector::scoreEvidence(const SSOEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasSSOBranch)  s += 0.55f;
    if (ev.hasInlinePath) s += 0.25f;
    if (ev.hasHeapPath)   s += 0.20f;
    return s > 1.0f ? 1.0f : s;
}

ContainerResult StringDetector::detect(const ssa::SSAFunction& fn) const {
    ContainerResult result;
    result.kind = ContainerKind::String;

    auto ev = analyseStructure(fn);
    result.confidence = ev.confidence;

    if (ev.confidence < 0.10f) return result;

    result.emittedType = "std::string";
    result.elementType.kind = RecoveredType::Kind::Int8;
    result.elementType.byteWidth = 1;

    // Infer compiler from SSO threshold.
    if (ev.ssoThreshold == 15) result.compilerVariant = CompilerVariant::GCC;
    else if (ev.ssoThreshold == 22 || ev.ssoThreshold == 23)
        result.compilerVariant = CompilerVariant::Clang;

    if (ev.hasHeapPath || ev.hasInlinePath) {
        AccessPattern ap;
        ap.kind    = AccessKind::SizeCheck;
        ap.emitted = "s.size()";
        result.accessPatterns.push_back(ap);
    }
    {
        AccessPattern ap;
        ap.kind    = AccessKind::Iterate;
        ap.emitted = "for (char c : s)";
        result.accessPatterns.push_back(ap);
    }

    return result;
}

// ─── SharedPtrDetector ───────────────────────────────────────────────────────

SharedPtrEvidence SharedPtrDetector::analyseStructure(const ssa::SSAFunction& fn) const {
    SharedPtrEvidence ev;
    ev.hasTwoPointers       = countOp(fn, ssa::IrInstr::Op::Load) >= 2;
    ev.hasAtomicDecrement   = hasAtomicOperation(fn);
    ev.hasZeroCheckFree     = hasZeroCheckFree(fn);
    ev.hasAtomicIncrement   = ev.hasAtomicDecrement &&
                              countOp(fn, ssa::IrInstr::Op::Add) >= 1;
    ev.found = ev.hasTwoPointers && ev.hasAtomicDecrement;
    ev.confidence = scoreEvidence(ev);
    return ev;
}

float SharedPtrDetector::scoreEvidence(const SharedPtrEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasTwoPointers)      s += 0.25f;
    if (ev.hasAtomicDecrement)  s += 0.30f;
    if (ev.hasZeroCheckFree)    s += 0.30f;
    if (ev.hasAtomicIncrement)  s += 0.15f;
    return s > 1.0f ? 1.0f : s;
}

ContainerResult SharedPtrDetector::detect(const ssa::SSAFunction& fn) const {
    ContainerResult result;
    result.kind = ContainerKind::SharedPtr;

    auto ev = analyseStructure(fn);
    result.confidence = ev.confidence;

    if (ev.confidence < 0.10f) return result;

    result.emittedType = "std::shared_ptr<T>";
    result.elementType.kind = RecoveredType::Kind::Pointer;

    {
        AccessPattern ap;
        ap.kind    = AccessKind::Lookup;
        ap.emitted = "sp.get()";
        result.accessPatterns.push_back(ap);
    }
    if (ev.hasZeroCheckFree) {
        AccessPattern ap;
        ap.kind    = AccessKind::Erase;
        ap.emitted = "sp.reset()";
        result.accessPatterns.push_back(ap);
    }

    return result;
}

} // namespace container_detect
} // namespace retdec
