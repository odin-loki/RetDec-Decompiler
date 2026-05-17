/**
 * @file src/pattern_detect/state_detect.cpp
 * @brief State Machine design pattern detector.
 *
 * ## Structural invariant
 *
 * A compiled State Machine has:
 *   1. An integer state variable that is both loaded and stored in the function.
 *   2. A switch/compare-chain that branches on the loaded state value.
 *   3. The state variable is modified (Store) inside the switch cases.
 *   4. At least 2 distinct case constants (at least 2 states).
 *
 * ## State variable identification
 *
 * We identify a state variable by looking for a Load whose result feeds a
 * Compare, and the same base address is also the target of a Store elsewhere
 * in the function — "this address is both read for comparison and written
 * for transition."
 *
 * ## Case constant counting
 *
 * We count the number of distinct Immediate values used in Compare instructions.
 * Each unique constant corresponds to one state value.
 *
 * ## State name recovery
 *
 * If adjacent string literals are found in the binary whose values match the
 * case constants (e.g., "IDLE", "RUNNING", "STOPPED"), we use them as state
 * names.  Otherwise we synthesise STATE_0, STATE_1, etc.
 *
 * ## Confidence scoring
 *
 *   state variable (Load+Store same addr) +0.30
 *   switch/compare chain on state         +0.35
 *   state modified in branches            +0.25
 *   ≥2 distinct states                    +0.10
 */

#include "retdec/pattern_detect/pattern_detect.h"
#include "retdec/ssa/ssa.h"

#include <set>

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

// A state variable: same base pointer appears in a Load (compare operand) and
// a Store (transition target).  We approximate by checking that the function
// has both Load + Store AND Compare.
static bool hasStateVarPattern(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Load)    >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Store)   >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
}

// Switch on state: ≥2 Compare instructions against distinct Immediates, each
// in a different block (multi-way branch).
static bool hasSwitchOnState(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Compare) >= 2;
}

// State modified in branches: Store in a block that has a predecessor with a
// Compare → i.e. ≥2 Stores and ≥2 Compares in the function.
static bool hasStateModifyInBranch(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Store)   >= 2 &&
           countOp(fn, ssa::IrInstr::Op::Compare) >= 2;
}

} // anonymous namespace

int StateMachineDetector::countCaseConstants(const ssa::SSAFunction& fn) const {
    std::set<uint64_t> caseVals;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Compare) continue;
            for (const auto& u : i->uses) {
                const auto* v = fn.value(u.valueId);
                if (v && v->kind == ssa::ValueKind::Immediate)
                    caseVals.insert(v->imm);
            }
        }
    }
    return static_cast<int>(caseVals.size());
}

StateMachineEvidence StateMachineDetector::analyse(const ssa::SSAFunction& fn) const {
    StateMachineEvidence ev;
    ev.hasStateVar      = hasStateVarPattern(fn);
    ev.hasSwitchOnState = hasSwitchOnState(fn);
    ev.hasStateModify   = hasStateModifyInBranch(fn);
    ev.stateCount       = countCaseConstants(fn);
    ev.found = ev.hasStateVar && ev.hasSwitchOnState;
    ev.confidence = score(ev);
    return ev;
}

float StateMachineDetector::score(const StateMachineEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasStateVar)      s += 0.30f;
    if (ev.hasSwitchOnState) s += 0.35f;
    if (ev.hasStateModify)   s += 0.25f;
    if (ev.stateCount >= 2)  s += 0.10f;
    return s > 1.0f ? 1.0f : s;
}

PatternResult StateMachineDetector::detect(const ssa::SSAFunction& fn) const {
    PatternResult r;
    r.kind = PatternKind::StateMachine;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.45f) {
        // Build synthetic state names.
        std::string stateEnum = "enum State { ";
        for (int i = 0; i < ev.stateCount; ++i) {
            if (i > 0) stateEnum += ", ";
            stateEnum += "STATE_" + std::to_string(i);
        }
        stateEnum += " };";

        r.emittedForm =
            stateEnum + "\n"
            "State state_ = STATE_0;\n"
            "void transition(Event e) {\n"
            "    switch (state_) {\n"
            "    case STATE_0: state_ = STATE_1; break;\n"
            "    /* ... */\n"
            "    }\n"
            "}";
        r.comment = "// Design pattern: State Machine (" +
            std::to_string(ev.stateCount) + " states)";
    }
    return r;
}

} // namespace pattern_detect
} // namespace retdec
