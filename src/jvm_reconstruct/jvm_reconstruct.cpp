/**
 * @file src/jvm_reconstruct/jvm_reconstruct.cpp
 * @brief JVM reconstruction pipeline — top-level orchestration.
 */

#include "retdec/jvm_reconstruct/jvm_reconstruct.h"

#include <algorithm>

namespace retdec {
namespace jvm_reconstruct {

using namespace bc_module;

// ─── JvmReconstructor ────────────────────────────────────────────────────────

JvmReconstructor::JvmReconstructor(ReconstructOptions opts)
    : opts_(opts) {}

// ─── CFG rewriting ───────────────────────────────────────────────────────────

BcInstruction JvmReconstructor::makeStore(uint32_t localIdx, uint32_t slotId,
                                           const StackSimResult& sim,
                                           uint32_t instrOffset) const {
    BcInstruction store;
    store.id     = UINT32_MAX; // synthetic
    store.offset = instrOffset;
    store.opcode = BcOpcode::StoreLocal;
    store.operands.push_back(BcLocalOperand{localIdx});
    // Carry the defining slot id as an additional int operand for traceability.
    store.operands.push_back(BcIntOperand{static_cast<int64_t>(slotId)});
    (void)sim;
    return store;
}

BcInstruction JvmReconstructor::makeLoad(uint32_t localIdx,
                                          const BcType& type,
                                          uint32_t instrOffset) const {
    BcInstruction load;
    load.id     = UINT32_MAX;
    load.offset = instrOffset;
    load.opcode = BcOpcode::LoadLocal;
    load.operands.push_back(BcLocalOperand{localIdx});
    load.operands.push_back(BcTypeOperand{type});
    return load;
}

void JvmReconstructor::rewriteCFG(BcCFG& cfg,
                                   BcMethod& method,
                                   const StackSimResult& sim,
                                   const CoalesceResult& coalesce,
                                   const LocalRebuildResult& locals) {
    // For each instruction that produces a surviving stack slot, insert a
    // StoreLocal after it.  For each instruction that consumes a surviving
    // slot, insert a LoadLocal before it.
    //
    // Coalesced (eliminated) slots are skipped — the expression flows inline.

    for (auto& blk : cfg.blocks()) {
        std::vector<BcInstruction> newInstrs;
        newInstrs.reserve(blk.instrs.size() * 2);

        for (auto& insn : blk.instrs) {
            auto infoIt = sim.instrInfo.find(insn.id);
            if (infoIt == sim.instrInfo.end()) {
                newInstrs.push_back(std::move(insn));
                continue;
            }
            const InstrStackInfo& info = infoIt->second;

            // Insert loads for consumed (popped) slots that survive.
            // We insert them before this instruction, in reverse pop order
            // (so the top-of-stack slot is the last load before the insn).
            std::vector<BcInstruction> loads;
            for (uint32_t sid : info.pops) {
                if (coalesce.eliminatedSlots.count(sid)) continue;
                auto localIt = locals.slotToLocal.find(sid);
                if (localIt == locals.slotToLocal.end()) continue;
                uint32_t localIdx = localIt->second;
                BcType ty = (localIdx < locals.locals.size())
                            ? locals.locals[localIdx].type
                            : types::Int();
                loads.push_back(makeLoad(localIdx, ty, insn.offset));
            }
            for (auto& ld : loads)
                newInstrs.push_back(std::move(ld));

            // The original instruction.
            newInstrs.push_back(std::move(insn));

            // Insert stores for produced (pushed) slots that survive.
            for (uint32_t sid : info.pushes) {
                if (coalesce.eliminatedSlots.count(sid)) continue;
                auto localIt = locals.slotToLocal.find(sid);
                if (localIt == locals.slotToLocal.end()) continue;
                uint32_t localIdx = localIt->second;
                newInstrs.push_back(
                    makeStore(localIdx, sid, sim, newInstrs.back().offset));
            }
        }

        blk.instrs = std::move(newInstrs);
    }

    // Populate method.locals from reconstruction result.
    method.locals = locals.locals;
    (void)method;
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

ReconstructResult JvmReconstructor::reconstruct(
        BcMethod& method,
        const std::vector<LVTEntry>& lvtEntries) {
    ReconstructResult result;

    // Phase 1+2: Stack simulation.
    JvmStackSim stackSim(opts_.stackSim);
    result.stackSim = stackSim.simulate(method.cfg, method);
    if (result.stackSim.status != StackSimResult::OK) {
        result.status = ReconstructResult::Error;
        result.error  = result.stackSim.error;
        return result;
    }

    // Phase 3: Slot coalescing.
    SlotCoalescer coalescer;
    result.coalesce = coalescer.coalesce(method.cfg, result.stackSim);

    // Phase 4: Local variable reconstruction.
    LocalRebuilder rebuilder(opts_.locals);
    result.locals = rebuilder.rebuild(method, method.cfg,
                                      result.stackSim,
                                      result.coalesce,
                                      lvtEntries);
    if (result.locals.status != LocalRebuildResult::OK) {
        result.status = ReconstructResult::PartialError;
        result.warnings.push_back(result.locals.error);
    }

    // Phase 4b: Exception variable introduction.
    ExceptionVarIntroducer ehIntro;
    ehIntro.introduce(method.cfg, method, result.stackSim, result.locals);

    // Phase 5: Pattern detection.
    if (opts_.detectPatterns) {
        PatternLifter patLifter;
        result.patterns = patLifter.lift(method.cfg, method,
                                          result.stackSim, result.locals);
    }

    // Phase 6: CFG rewriting.
    if (opts_.rewriteCFG) {
        rewriteCFG(method.cfg, method,
                   result.stackSim, result.coalesce, result.locals);
    }

    return result;
}

// ─── reconstructModule ────────────────────────────────────────────────────────

int JvmReconstructor::reconstructModule(BcModule& module) {
    int errors = 0;
    for (auto& cls : module.classes()) {
        for (auto& method : cls.methods) {
            if (method.cfg.blocks().empty()) continue;
            auto result = reconstruct(method);
            if (result.status == ReconstructResult::Error)
                ++errors;
        }
    }
    return errors;
}

} // namespace jvm_reconstruct
} // namespace retdec
