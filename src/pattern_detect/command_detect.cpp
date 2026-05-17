/**
 * @file src/pattern_detect/command_detect.cpp
 * @brief Command design pattern detector.
 *
 * ## Structural invariant
 *
 * A compiled Command pattern has:
 *   1. A vtable with a dominant `execute` method (virtual call at vtable slot 0
 *      or 1, accounting for the destructor slot).
 *   2. A container (queue/stack/vector) of base-class pointers to ICommand.
 *   3. A loop over the container calling the virtual execute() method.
 *   4. Optional: a second vtable method `undo()` for command history.
 *
 * ## IR signals
 *
 * `hasVtableExecute`:
 *   - A Load from a vtable-like structure (load of a pointer from offset 0–16),
 *     followed by an indirect Call through the loaded pointer.
 *
 * `hasContainerOfPtrs`:
 *   - A Load + Store of pointer-typed values into a container field.
 *   - Or a push_back call storing a command pointer.
 *
 * `hasLoopExecute`:
 *   - A loop (back-edge) with an indirect call (the virtual execute()).
 *
 * `hasUndo`:
 *   - An indirect call from vtable slot 2 or higher (the undo() slot).
 *   - Or a callee name containing "undo" / "rollback".
 *
 * ## Confidence scoring
 *
 *   vtable execute call         +0.35
 *   container of command ptrs   +0.30
 *   loop executing commands     +0.25
 *   undo method (bonus)         +0.10
 */

#include "retdec/pattern_detect/pattern_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace pattern_detect {

namespace {

static bool hasBackEdge(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (uint32_t s : blk->succs) if (s <= b) return true;
    }
    return false;
}

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

// Vtable execute: a Load followed by an indirect Call in the same block.
static bool hasVtableExecute(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        bool loadSeen = false;
        for (const auto* i : blk->instrs) {
            if (!i) continue;
            if (i->op == ssa::IrInstr::Op::Load) { loadSeen = true; continue; }
            if (loadSeen && i->op == ssa::IrInstr::Op::Call) {
                // Accept an indirect call (empty callee or '*' prefix) or
                // a call whose name suggests execute.
                const auto& cn = i->calleeName;
                if (cn.empty() || cn[0] == '*' ||
                    cn.find("execute") != std::string::npos ||
                    cn.find("Execute") != std::string::npos ||
                    cn.find("run")     != std::string::npos)
                    return true;
            }
        }
    }
    return false;
}

// Container of ptrs: push_back of a command pointer or Load+Store of ptrs.
static bool hasContainerOfPtrs(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = i->calleeName;
            if (cn.find("push_back")    != std::string::npos ||
                cn.find("emplace_back") != std::string::npos ||
                cn.find("enqueue")      != std::string::npos ||
                cn.find("push")         != std::string::npos)
                return true;
        }
    }
    return countOp(fn, ssa::IrInstr::Op::Load)  >= 2 &&
           countOp(fn, ssa::IrInstr::Op::Store) >= 1;
}

} // anonymous namespace

bool CommandDetector::hasUndoMethod(const ssa::SSAFunction& fn) const {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = i->calleeName;
            if (cn.find("undo")     != std::string::npos ||
                cn.find("Undo")     != std::string::npos ||
                cn.find("rollback") != std::string::npos ||
                cn.find("revert")   != std::string::npos)
                return true;
        }
    }
    return false;
}

CommandEvidence CommandDetector::analyse(const ssa::SSAFunction& fn) const {
    CommandEvidence ev;
    ev.hasVtableExecute   = hasVtableExecute(fn);
    ev.hasContainerOfPtrs = hasContainerOfPtrs(fn);
    ev.hasLoopExecute     = hasBackEdge(fn) && ev.hasVtableExecute;
    ev.hasUndo            = hasUndoMethod(fn);
    ev.found = ev.hasVtableExecute;
    ev.confidence = score(ev);
    return ev;
}

float CommandDetector::score(const CommandEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasVtableExecute)   s += 0.35f;
    if (ev.hasContainerOfPtrs) s += 0.30f;
    if (ev.hasLoopExecute)     s += 0.25f;
    if (ev.hasUndo)            s += 0.10f;
    return s > 1.0f ? 1.0f : s;
}

PatternResult CommandDetector::detect(const ssa::SSAFunction& fn) const {
    PatternResult r;
    r.kind = PatternKind::Command;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    r.hasVariant  = ev.hasUndo;
    r.variantName = ev.hasUndo ? "with-undo" : "";
    if (ev.confidence >= 0.45f) {
        r.emittedForm =
            "struct ICommand { virtual void execute() = 0; };\n"
            "std::queue<ICommand*> history;\n"
            "void runAll() {\n"
            "    while (!history.empty()) {\n"
            "        history.front()->execute();\n"
            "        history.pop();\n"
            "    }\n"
            "}";
        r.comment = "// Design pattern: Command" +
            std::string(ev.hasUndo ? " (with undo support)" : "");
    }
    return r;
}

} // namespace pattern_detect
} // namespace retdec
