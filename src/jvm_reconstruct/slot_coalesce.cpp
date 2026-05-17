/**
 * @file src/jvm_reconstruct/slot_coalesce.cpp
 * @brief Stack-slot coalescing implementation.
 */

#include "retdec/jvm_reconstruct/slot_coalesce.h"

#include <algorithm>

namespace retdec {
namespace jvm_reconstruct {

using namespace bc_module;

// ─── Build use / def count maps ──────────────────────────────────────────────

std::unordered_map<uint32_t, uint32_t>
SlotCoalescer::buildUseCount(const BcCFG& cfg,
                              const StackSimResult& simResult) const {
    std::unordered_map<uint32_t, uint32_t> uses;
    for (const auto& blk : cfg.blocks()) {
        for (const auto& insn : blk.instrs) {
            auto it = simResult.instrInfo.find(insn.id);
            if (it == simResult.instrInfo.end()) continue;
            for (uint32_t sid : it->second.pops)
                ++uses[sid];
        }
    }
    return uses;
}

std::unordered_map<uint32_t, uint32_t>
SlotCoalescer::buildDefCount(const BcCFG& cfg,
                              const StackSimResult& simResult) const {
    std::unordered_map<uint32_t, uint32_t> defs;
    for (const auto& blk : cfg.blocks()) {
        for (const auto& insn : blk.instrs) {
            auto it = simResult.instrInfo.find(insn.id);
            if (it == simResult.instrInfo.end()) continue;
            for (uint32_t sid : it->second.pushes)
                ++defs[sid];
        }
    }
    return defs;
}

// ─── Cross-block check ────────────────────────────────────────────────────────

bool SlotCoalescer::isCrossBlock(uint32_t slotId,
                                  const BcCFG& cfg,
                                  const StackSimResult& simResult) const {
    // Check if slot appears in any block's entry state (other than as a
    // phi-slot which is defined there).
    for (const auto& kv : simResult.blockEntryStates) {
        const auto& state = kv.second;
        for (uint32_t sid : state.slotIds) {
            if (sid == slotId)
                return true;
        }
    }
    return false;
}

// ─── Main coalescing pass ─────────────────────────────────────────────────────

CoalesceResult SlotCoalescer::coalesce(const BcCFG& cfg,
                                        const StackSimResult& simResult) {
    CoalesceResult result;

    auto useCnt = buildUseCount(cfg, simResult);
    auto defCnt = buildDefCount(cfg, simResult);

    // Map: slot id → instruction id that defines it (block-local defs only).
    // We also need: slot id → block that contains the def.
    std::unordered_map<uint32_t, uint32_t> slotDefInstr;  // slot → instr id
    std::unordered_map<uint32_t, uint32_t> slotDefBlock;  // slot → block id

    for (const auto& blk : cfg.blocks()) {
        for (const auto& insn : blk.instrs) {
            auto it = simResult.instrInfo.find(insn.id);
            if (it == simResult.instrInfo.end()) continue;
            for (uint32_t sid : it->second.pushes) {
                slotDefInstr[sid] = insn.id;
                slotDefBlock[sid] = blk.id;
            }
        }
    }

    // Map: slot id → instruction id that uses it.
    std::unordered_map<uint32_t, uint32_t> slotUseInstr;
    std::unordered_map<uint32_t, uint32_t> slotUseBlock;

    for (const auto& blk : cfg.blocks()) {
        for (const auto& insn : blk.instrs) {
            auto it = simResult.instrInfo.find(insn.id);
            if (it == simResult.instrInfo.end()) continue;
            for (uint32_t sid : it->second.pops) {
                slotUseInstr[sid] = insn.id;
                slotUseBlock[sid] = blk.id;
            }
        }
    }

    // Determine which slots can be eliminated.
    for (const auto& slot : simResult.slots) {
        uint32_t sid = slot.id;

        // Must have exactly one definition.
        if (defCnt.count(sid) == 0 || defCnt.at(sid) != 1) {
            result.survivingSlots.push_back(sid);
            continue;
        }

        // Must have exactly one use.
        if (useCnt.count(sid) == 0 || useCnt.at(sid) != 1) {
            result.survivingSlots.push_back(sid);
            continue;
        }

        // Must not live across a block boundary.
        if (isCrossBlock(sid, cfg, simResult)) {
            result.survivingSlots.push_back(sid);
            continue;
        }

        // Def and use must be in the same block.
        if (!slotDefBlock.count(sid) || !slotUseBlock.count(sid)) {
            result.survivingSlots.push_back(sid);
            continue;
        }
        if (slotDefBlock.at(sid) != slotUseBlock.at(sid)) {
            result.survivingSlots.push_back(sid);
            continue;
        }

        // Mark as eliminatable.
        result.eliminatedSlots.insert(sid);
        result.inlinedSlotToDef[sid] = slotDefInstr.at(sid);
    }

    return result;
}

} // namespace jvm_reconstruct
} // namespace retdec
