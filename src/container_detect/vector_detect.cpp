/**
 * @file src/container_detect/vector_detect.cpp
 * @brief std::vector<T> detector — three-layer analysis.
 *
 * ## Layer 1: Structure detection
 *
 * A compiled std::vector has three pointers stored at consecutive offsets of
 * a struct.  In IR this produces a cluster of three Load instructions that:
 *   - Access three consecutive pointer-width offsets (0, 8, 16 on 64-bit).
 *   - Are used in pointer-arithmetic expressions (Sub for size, Store for growth).
 *
 * Heuristic signals for Layer 1 (three-pointer layout):
 *   - At least 3 Loads from the same base register.
 *   - Sub instruction operating on two loaded pointer values (size = end - begin).
 *   - The loaded values are used in pointer-arithmetic (Add / Mul for indexing).
 *
 * ## Layer 2: Context validation
 *
 * Growth pattern (push_back → reallocation):
 *   1. A Compare of current size vs capacity (end >= capacity_end).
 *   2. A Call to malloc/realloc, or an inline new[] expression.
 *   3. A copy/memmove loop moving old elements to the new buffer.
 *   4. A Call to free or delete[], or an inline deallocation.
 *
 * Growth factor:
 *   - GCC libstdc++: `new_cap = 2 * old_cap`  — Mul by 2 or Shl by 1.
 *   - Clang libc++:  same as GCC.
 *   - MSVC STL:      `new_cap = old_cap + old_cap / 2`  — Add + Shr by 1.
 *
 * ## Layer 3: Access pattern emission
 *
 *   v.push_back(x)  — insert path (alloc + grow + store)
 *   v.size()        — Sub(end, begin) / sizeof(T)
 *   v[i]            — Load from begin + i * sizeof(T)
 *   v.begin()       — the begin pointer itself
 *   v.end()         — the end pointer itself
 *
 * ## Confidence scoring
 *
 *   three Load cluster             +0.30
 *   Sub(end, begin) for size       +0.25
 *   growth pattern (alloc + free)  +0.25
 *   element access (begin[i])      +0.20
 */

#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

#include <sstream>

namespace retdec {
namespace container_detect {

namespace {

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

// Three-pointer layout: does the function have ≥3 Loads + at least one Sub
// (end - begin) + at least one Add (begin + index)?
static bool hasThreePointerLayout(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Load)  >= 3 &&
           countOp(fn, ssa::IrInstr::Op::Sub)   >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Add)   >= 1;
}

// Growth pattern: malloc/realloc/new call + memmove/copy loop + free/delete call.
static bool hasGrowthPattern(const ssa::SSAFunction& fn,
                              float& growthFactor,
                              CompilerVariant& variant) {
    bool hasMalloc = false, hasFree = false, hasCopy = false, hasShl = false, hasShr = false;

    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Call) {
                const auto& cn = instr->calleeName;
                if (cn == "malloc"  || cn == "realloc"  || cn == "_Znwm" ||
                    cn == "calloc"  || cn.find("allocate") != std::string::npos)
                    hasMalloc = true;
                if (cn == "free"    || cn == "_ZdlPv"   || cn == "operator delete" ||
                    cn.find("deallocate") != std::string::npos)
                    hasFree = true;
                if (cn == "memmove" || cn == "memcpy"   ||
                    cn.find("copy") != std::string::npos)
                    hasCopy = true;
            }
            if (instr->op == ssa::IrInstr::Op::Shl) {
                // Shl(cap, 1) → new_cap = 2 * old_cap → GCC/Clang growth
                for (const auto& u : instr->uses) {
                    const auto* sv = fn.value(u.valueId);
                    if (sv && sv->kind == ssa::ValueKind::Immediate && sv->imm == 1)
                        { hasShl = true; break; }
                }
            }
            if (instr->op == ssa::IrInstr::Op::Shr) {
                // Shr(cap, 1) → cap / 2 → new_cap = cap + cap/2 → MSVC growth
                for (const auto& u : instr->uses) {
                    const auto* sv = fn.value(u.valueId);
                    if (sv && sv->kind == ssa::ValueKind::Immediate && sv->imm == 1)
                        { hasShr = true; break; }
                }
            }
        }
    }

    if (hasMalloc && hasFree) {
        if (hasShl) {
            growthFactor = 2.0f;
            variant = CompilerVariant::GCC;
        } else if (hasShr) {
            growthFactor = 1.5f;
            variant = CompilerVariant::MSVC;
        } else {
            growthFactor = 2.0f;
            variant = CompilerVariant::Unknown;
        }
        return true;
    }
    return false;
}

// Element access: Load from begin + i * stride (where stride is a small constant).
static bool hasElementAccess(const ssa::SSAFunction& fn,
                              uint8_t& elementByteWidth) {
    // We look for a Mul instruction with a small constant stride (1, 2, 4, 8)
    // which is used in an Add (base + offset) that feeds a Load.
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Mul) continue;
            for (const auto& use : instr->uses) {
                const auto* sv = fn.value(use.valueId);
                if (!sv || sv->kind != ssa::ValueKind::Immediate) continue;
                uint64_t imm = sv->imm;
                if (imm == 1 || imm == 2 || imm == 4 || imm == 8) {
                    elementByteWidth = static_cast<uint8_t>(imm);
                    return true;
                }
            }
        }
    }
    return false;
}

} // anonymous namespace

// ─── VectorDetector ───────────────────────────────────────────────────────────

VectorEvidence VectorDetector::analyseStructure(const ssa::SSAFunction& fn) const {
    VectorEvidence ev;
    ev.hasBeginEndCap   = hasThreePointerLayout(fn);
    ev.hasSizeArith     = countOp(fn, ssa::IrInstr::Op::Sub) >= 1 &&
                          countOp(fn, ssa::IrInstr::Op::Load) >= 2;

    float gf = 0.0f; CompilerVariant cv = CompilerVariant::Unknown;
    ev.hasGrowthPattern = hasGrowthPattern(fn, gf, cv);
    ev.growthFactor     = gf;
    ev.hasIndexAccess   = hasElementAccess(fn, ev.elementByteWidth);
    ev.found = ev.hasBeginEndCap || ev.hasGrowthPattern;
    ev.confidence = scoreEvidence(ev);
    return ev;
}

float VectorDetector::scoreEvidence(const VectorEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasBeginEndCap)   s += 0.30f;
    if (ev.hasSizeArith)     s += 0.25f;
    if (ev.hasGrowthPattern) s += 0.25f;
    if (ev.hasIndexAccess)   s += 0.20f;
    return s > 1.0f ? 1.0f : s;
}

CompilerVariant VectorDetector::detectVariant(const ssa::SSAFunction& fn,
                                               const VectorEvidence& ev) const {
    if (ev.growthFactor >= 1.9f) {
        const std::string& n = fn.name();
        if (n.find("_VSTD") != std::string::npos) return CompilerVariant::Clang;
        return CompilerVariant::GCC;
    }
    if (ev.growthFactor >= 1.4f) return CompilerVariant::MSVC;
    return CompilerVariant::Unknown;
}

std::vector<AccessPattern> VectorDetector::recoverAccessPatterns(
        const ssa::SSAFunction& fn) const {
    std::vector<AccessPattern> patterns;

    // Push-back: growth pattern present.
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Store) continue;
            AccessPattern ap;
            ap.kind = AccessKind::PushBack;
            ap.instrId = instr->id;
            ap.emitted = "v.push_back(elem)";
            patterns.push_back(ap);
            break;
        }
        if (!patterns.empty()) break;
    }

    // Size check: Sub present.
    if (countOp(fn, ssa::IrInstr::Op::Sub) >= 1) {
        AccessPattern ap;
        ap.kind = AccessKind::SizeCheck;
        ap.emitted = "v.size()";
        patterns.push_back(ap);
    }

    // Iterate: a loop with Load from begin pointer.
    if (countOp(fn, ssa::IrInstr::Op::Load) >= 2 &&
        countOp(fn, ssa::IrInstr::Op::Add)  >= 1) {
        AccessPattern ap;
        ap.kind = AccessKind::Iterate;
        ap.emitted = "for (auto& e : v)";
        patterns.push_back(ap);
    }

    return patterns;
}

std::string VectorDetector::emitType(const RecoveredType& elem) const {
    if (elem.kind == RecoveredType::Kind::Unknown) return "std::vector<int>";
    return "std::vector<" + elem.toString() + ">";
}

ContainerResult VectorDetector::detect(const ssa::SSAFunction& fn) const {
    ContainerResult result;
    result.kind = ContainerKind::Vector;

    auto ev = analyseStructure(fn);
    result.confidence = ev.confidence;

    if (ev.confidence < 0.10f) return result;

    result.compilerVariant = detectVariant(fn, ev);
    result.accessPatterns  = recoverAccessPatterns(fn);

    // Emit type with recovered element width.
    RecoveredType elem;
    if (ev.elementByteWidth > 0) {
        switch (ev.elementByteWidth) {
        case 1: elem.kind = RecoveredType::Kind::Int8;  elem.byteWidth = 1; break;
        case 2: elem.kind = RecoveredType::Kind::Int16; elem.byteWidth = 2; break;
        case 4: elem.kind = RecoveredType::Kind::Int32; elem.byteWidth = 4; break;
        case 8: elem.kind = RecoveredType::Kind::Int64; elem.byteWidth = 8; break;
        default: break;
        }
    }
    result.elementType = elem;
    result.emittedType = emitType(elem);

    return result;
}

} // namespace container_detect
} // namespace retdec
