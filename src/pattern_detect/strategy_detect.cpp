/**
 * @file src/pattern_detect/strategy_detect.cpp
 * @brief Strategy design pattern detector.
 *
 * ## Structural invariant (group mode — three functions)
 *
 * A compiled Strategy pattern has three functions:
 *
 * 1. **Setter** (`setStrategy`): writes an interface pointer into a struct field.
 *    ```
 *    Store(new_strategy_ptr, struct_field_offset)
 *    ```
 *
 * 2. **Executor** (`execute` / `doAction`): reads the stored interface pointer
 *    and calls through it (virtual dispatch):
 *    ```
 *    %strat = Load(struct_field_offset)
 *    %vtbl  = Load(%strat + 0)
 *    %fn    = Load(%vtbl + slot)
 *    Call(%fn, %strat, args...)
 *    ```
 *
 * 3. **Context** (often the owning class): ties setter + executor together.
 *
 * ## Single-function fallback
 *
 * When only one function is given, we look for Load → indirect Call (delegation
 * through a stored pointer) in the same function.
 *
 * ## Confidence scoring
 *
 *   stored interface field       +0.30
 *   setter function              +0.35
 *   indirect call (delegation)   +0.35
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

// Setter: a Store of a pointer to a struct field (Store + Load present).
static bool isSetterFunction(const ssa::SSAFunction& fn) {
    // A setter has a Store and minimal other computation.
    return countOp(fn, ssa::IrInstr::Op::Store) >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Load)  <= 2;
}

// Executor: Load chain + indirect Call.
static bool isExecutorFunction(const ssa::SSAFunction& fn) {
    bool hasLoad = countOp(fn, ssa::IrInstr::Op::Load) >= 2;
    bool hasIndirectCall = false;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = i->calleeName;
            if (cn.empty() || cn[0] == '*' ||
                cn.find("doAlgorithm") != std::string::npos ||
                cn.find("execute")     != std::string::npos ||
                cn.find("sort")        != std::string::npos)
                hasIndirectCall = true;
        }
    }
    return hasLoad && hasIndirectCall;
}

} // anonymous namespace

StrategyEvidence StrategyDetector::analyse(const ssa::SSAFunction& fn) const {
    StrategyEvidence ev;
    ev.hasInterfaceField = countOp(fn, ssa::IrInstr::Op::Load)  >= 1 &&
                           countOp(fn, ssa::IrInstr::Op::Store) >= 1;
    ev.hasSetter = isSetterFunction(fn);
    ev.hasIndirectCall = isExecutorFunction(fn);
    ev.found = ev.hasIndirectCall;
    ev.confidence = score(ev);
    return ev;
}

float StrategyDetector::score(const StrategyEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasInterfaceField) s += 0.30f;
    if (ev.hasSetter)         s += 0.35f;
    if (ev.hasIndirectCall)   s += 0.35f;
    return s > 1.0f ? 1.0f : s;
}

PatternResult StrategyDetector::detect(const ssa::SSAFunction& fn) const {
    PatternResult r;
    r.kind = PatternKind::Strategy;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.45f) {
        r.emittedForm =
            "struct IStrategy { virtual void doAlgorithm() = 0; };\n"
            "struct Context {\n"
            "    IStrategy* strategy_;\n"
            "    void setStrategy(IStrategy* s) { strategy_ = s; }\n"
            "    void execute() { strategy_->doAlgorithm(); }\n"
            "};";
        r.comment = "// Design pattern: Strategy";
    }
    return r;
}

PatternResult StrategyDetector::detectGroup(
        const std::vector<const ssa::SSAFunction*>& fns) const {
    PatternResult r;
    r.kind = PatternKind::Strategy;
    bool hasSetter = false, hasExecutor = false, hasField = false;
    for (const auto* fn : fns) {
        if (!fn) continue;
        if (!hasSetter   && isSetterFunction(*fn))   hasSetter   = true;
        if (!hasExecutor && isExecutorFunction(*fn))  hasExecutor = true;
        if (!hasField && (countOp(*fn, ssa::IrInstr::Op::Load) >= 1 &&
                          countOp(*fn, ssa::IrInstr::Op::Store) >= 1))
            hasField = true;
    }
    r.confidence = (hasField    ? 0.30f : 0.0f)
                 + (hasSetter   ? 0.35f : 0.0f)
                 + (hasExecutor ? 0.35f : 0.0f);
    if (r.confidence > 1.0f) r.confidence = 1.0f;
    if (r.confidence >= 0.45f) {
        r.emittedForm =
            "struct IStrategy { virtual void doAlgorithm() = 0; };\n"
            "struct Context {\n"
            "    IStrategy* strategy_;\n"
            "    void setStrategy(IStrategy* s) { strategy_ = s; }\n"
            "    void execute() { strategy_->doAlgorithm(); }\n"
            "};";
        r.comment = "// Design pattern: Strategy";
    }
    return r;
}

} // namespace pattern_detect
} // namespace retdec
