/**
 * @file src/pattern_detect/factory_detect.cpp
 * @brief Factory Method design pattern detector.
 *
 * ## Structural invariant
 *
 * A compiled factory function has:
 *   1. A switch instruction (or an if-else chain) over a single integer
 *      discriminant (the "type" parameter).
 *   2. Each branch contains exactly one allocation (malloc / operator new).
 *   3. All branches converge to a single return point, returning through a
 *      common base pointer type.
 *   4. At least two distinct allocation call-sites (two or more products).
 *
 * The switch can be:
 *   - An explicit IR Switch instruction (if the backend lowers it that way).
 *   - A cascade of Compare + conditional Branch instructions (if-else chain).
 *
 * ## Multi-block layout
 *
 * ```
 * entry:  Compare(type, 0) → blk0 | Compare(type, 1) → blk1 | default
 * blk0:   %p = new ConcreteA(); Br exit
 * blk1:   %p = new ConcreteB(); Br exit
 * exit:   Ret %p
 * ```
 *
 * In IR we approximate this by counting:
 *   - Blocks with ≥1 allocation + ≥1 return/branch-to-exit.
 *   - A Compare instruction in the entry block (discriminant switch).
 *
 * ## Confidence scoring
 *
 *   switch/compare discriminant   +0.30
 *   ≥2 allocation sites           +0.40
 *   common return path            +0.30
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

// Count distinct allocation call-sites (malloc/new calls).
static int countAllocSites(const ssa::SSAFunction& fn) {
    int n = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = i->calleeName;
            if (cn == "malloc" || cn == "_Znwm" || cn == "operator new" ||
                cn.find("allocate") != std::string::npos)
                ++n;
        }
    }
    return n;
}

// A switch/compare discriminant: multiple Compares against distinct Immediate
// constants (the case values), each in different blocks.
static bool hasSwitchDiscriminant(const ssa::SSAFunction& fn) {
    // Accept ≥2 Compare instructions total across the function.
    return countOp(fn, ssa::IrInstr::Op::Compare) >= 2;
}

// Common return path: a single Ret or all blocks eventually converge to one Ret.
static bool hasCommonReturn(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Ret) >= 1;
}

} // anonymous namespace

FactoryEvidence FactoryDetector::analyse(const ssa::SSAFunction& fn) const {
    FactoryEvidence ev;
    ev.hasSwitchOrIfElse = hasSwitchDiscriminant(fn);
    ev.branchCount       = countOp(fn, ssa::IrInstr::Op::Compare);
    int allocSites       = countAllocSites(fn);
    ev.hasMultipleAllocs = allocSites >= 2;
    ev.hasBaseReturn     = hasCommonReturn(fn);
    ev.found = ev.hasMultipleAllocs && ev.hasSwitchOrIfElse;
    ev.confidence = score(ev);
    return ev;
}

float FactoryDetector::score(const FactoryEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasSwitchOrIfElse) s += 0.30f;
    if (ev.hasMultipleAllocs) s += 0.40f;
    if (ev.hasBaseReturn)     s += 0.30f;
    return s > 1.0f ? 1.0f : s;
}

PatternResult FactoryDetector::detect(const ssa::SSAFunction& fn) const {
    PatternResult r;
    r.kind = PatternKind::Factory;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.45f) {
        r.emittedForm =
            "Base* createProduct(int type) {\n"
            "    switch (type) {\n"
            "    case 0: return new ConcreteA();\n"
            "    case 1: return new ConcreteB();\n"
            "    default: return nullptr;\n"
            "    }\n"
            "}";
        r.comment = "// Design pattern: Factory Method";
    }
    return r;
}

} // namespace pattern_detect
} // namespace retdec
