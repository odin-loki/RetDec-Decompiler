/**
 * @file src/dce/live_root_collector.cpp
 * @brief C-semantic live root collection.
 *
 * Live roots are instructions whose effects are observable outside the function:
 *   - Return value writes (RET, and defs of return registers at RET points)
 *   - Stores through pointer arguments
 *   - Stores to global (non-stack) memory
 *   - CALLs and other side-effectful operations (SYSCALL, INT)
 */

#include "retdec/dce/dce.h"
#include "retdec/ssa/ssa.h"
#include "retdec/call_conv/call_conv.h"

namespace retdec {
namespace dce {

// ─── collectReturnRoots ───────────────────────────────────────────────────────

void LiveRootCollector::collectReturnRoots(const ssa::SSAFunction& fn,
                                             std::vector<LiveRoot>& roots) const {
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Ret) {
                LiveRoot r;
                r.instrId = instr->id;
                r.kind    = LiveRootKind::ReturnValue;
                roots.push_back(r);

                // Also mark all instructions that define values live-out
                // at this block (the return register defs).
                for (ssa::VarId var : blk->liveOut) {
                    // Walk all instructions in this block that define this var.
                    for (const ssa::IrInstr* def : blk->instrs) {
                        if (!def) continue;
                        const ssa::IrValue* defVal = fn.value(def->defValue);
                        if (defVal && defVal->varId == var) {
                            LiveRoot dr;
                            dr.instrId = def->id;
                            dr.kind    = LiveRootKind::ReturnValue;
                            roots.push_back(dr);
                        }
                    }
                }
            }
        }
    }
}

// ─── collectPtrArgWrites ──────────────────────────────────────────────────────

void LiveRootCollector::collectPtrArgWrites(
        const ssa::SSAFunction& fn,
        const call_conv::CallingConvention& cc,
        std::vector<LiveRoot>& roots) const {

    // Build a set of SSA value IDs that represent pointer arguments.
    std::unordered_set<ssa::ValueId> ptrArgValues;
    for (const auto& arg : cc.args) {
        if (arg.ssaValueId != UINT32_MAX) {
            ptrArgValues.insert(arg.ssaValueId);
        }
    }
    if (ptrArgValues.empty()) return;

    // Any Store whose address operand is (transitively) derived from a
    // pointer argument is a live root.
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Store) continue;
            // The first use of a Store is the destination address.
            if (instr->uses.empty()) continue;
            ssa::ValueId destId = instr->uses[0].valueId;
            if (ptrArgValues.count(destId)) {
                LiveRoot r;
                r.instrId = instr->id;
                r.kind    = LiveRootKind::PtrArgWrite;
                roots.push_back(r);
            }
        }
    }
}

// ─── collectGlobalWrites ──────────────────────────────────────────────────────

void LiveRootCollector::collectGlobalWrites(const ssa::SSAFunction& fn,
                                              std::vector<LiveRoot>& roots) const {
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Store) continue;
            // Check if the destination is a non-stack MemRef (global/heap).
            for (const auto& use : instr->uses) {
                const ssa::IrValue* v = fn.value(use.valueId);
                if (v && v->kind == ssa::ValueKind::MemRef && !v->memIsStack) {
                    LiveRoot r;
                    r.instrId = instr->id;
                    r.kind    = LiveRootKind::GlobalWrite;
                    roots.push_back(r);
                    break;
                }
            }
        }
    }
}

// ─── collectIoSideEffects ─────────────────────────────────────────────────────

void LiveRootCollector::collectIoSideEffects(const ssa::SSAFunction& fn,
                                               std::vector<LiveRoot>& roots) const {
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr) continue;
            // All CALLs are conservatively live (inter-proc analysis is separate).
            if (instr->op == ssa::IrInstr::Op::Call) {
                LiveRoot r;
                r.instrId = instr->id;
                r.kind    = LiveRootKind::IoSideEffect;
                roots.push_back(r);
            }
        }
    }
}

// ─── LiveRootCollector::run ───────────────────────────────────────────────────

std::vector<LiveRoot> LiveRootCollector::run(
        const ssa::SSAFunction& fn,
        const call_conv::CallingConvention& cc) const {

    std::vector<LiveRoot> roots;
    collectReturnRoots(fn, roots);
    collectPtrArgWrites(fn, cc, roots);
    collectGlobalWrites(fn, roots);
    collectIoSideEffects(fn, roots);

    // Deduplicate by instrId.
    std::unordered_set<InstrId> seen;
    std::vector<LiveRoot> unique;
    for (const auto& r : roots) {
        if (seen.insert(r.instrId).second) unique.push_back(r);
    }
    return unique;
}

} // namespace dce
} // namespace retdec
