/**
 * @file src/dce/dce_pass.cpp
 * @brief DcePass orchestrator + DeadCodeResult summary.
 *
 * Combines all four sub-passes:
 *   1. AbiArtifactMarker   — identifies ABI-level bookkeeping instructions
 *   2. LiveRootCollector   — identifies C-semantic live roots
 *   3. DeadPropagation     — backward propagation from roots
 *   4. UnreachableEliminator — forward reachability
 *
 * The result is analysis-only: the SSAFunction is not mutated.
 * The code generator uses DeadCodeResult::liveInstrs as the emit filter.
 */

#include "retdec/dce/dce.h"
#include "retdec/ssa/ssa.h"
#include "retdec/call_conv/call_conv.h"

#include <sstream>

namespace retdec {
namespace dce {

// ─── DeadCodeResult::summary ─────────────────────────────────────────────────

std::string DeadCodeResult::summary() const {
    std::ostringstream os;
    os << "DCE: " << eliminatedInstrCount << "/" << totalInstrs
       << " instrs eliminated ("
       << static_cast<int>(eliminationRate() * 100) << "%)"
       << ", " << eliminatedBlockCount << " unreachable blocks";
    if (!abiArtifactsRemoved.empty()) {
        os << ", ABI artifacts: ";
        for (const auto& [kind, cnt] : abiArtifactsRemoved) {
            if (cnt) os << abiArtifactKindName(kind) << "=" << cnt << " ";
        }
    }
    return os.str();
}

// ─── DcePass::run ─────────────────────────────────────────────────────────────

DeadCodeResult DcePass::run(const ssa::SSAFunction& fn,
                              const call_conv::CallingConvention& cc,
                              const Config& cfg) const {
    DeadCodeResult result;

    // Count total instructions.
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        result.totalInstrs += blk->instrs.size();
    }

    // ── Phase 1: ABI artifact marking ──────────────────────────────────────
    std::unordered_set<InstrId> abiArtifactSet;
    if (!cfg.keepAbiArtifacts) {
        AbiArtifactMarker marker;
        result.abiArtifacts = marker.run(fn, cfg.abiCfg);

        for (const AbiArtifact& art : result.abiArtifacts) {
            if (art.instrId != UINT32_MAX) {
                abiArtifactSet.insert(art.instrId);
                ++result.abiArtifactsRemoved[art.kind];
            }
            // For balanced callee-save pairs, also include the restore.
            if (art.kind == AbiArtifactKind::CalleeSavePair &&
                art.balanced &&
                art.pairedId != UINT32_MAX) {
                abiArtifactSet.insert(art.pairedId);
                ++result.abiArtifactsRemoved[art.kind];
            }
        }
    }

    // ── Phase 2: Live root collection ───────────────────────────────────────
    LiveRootCollector rootCollector;
    auto roots = rootCollector.run(fn, cc);

    // ── Phase 3: Backward liveness propagation ─────────────────────────────
    DeadPropagation prop;
    result.liveInstrs = prop.run(fn, roots, abiArtifactSet);

    // ── Phase 4: Unreachable block elimination ──────────────────────────────
    if (cfg.eliminateUnreachableBlocks) {
        UnreachableEliminator unreachable;
        result.eliminatedBlocks = unreachable.run(fn);
        result.eliminatedBlockCount = result.eliminatedBlocks.size();

        // Add all instructions in unreachable blocks to the dead set.
        for (BlockId blkId : result.eliminatedBlocks) {
            const ssa::BasicBlock* blk = fn.block(blkId);
            if (!blk) continue;
            for (const ssa::IrInstr* instr : blk->instrs) {
                if (instr) result.eliminatedInstrs.insert(instr->id);
            }
        }
    }

    // ── Phase 5: Assemble dead set ──────────────────────────────────────────
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        if (result.eliminatedBlocks.count(blk->id)) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr) continue;
            InstrId id = instr->id;
            bool isLive = result.liveInstrs.count(id) > 0;
            bool isAbiArtifact = abiArtifactSet.count(id) > 0;
            if (!isLive || isAbiArtifact) {
                result.eliminatedInstrs.insert(id);
            }
        }
    }

    // Remove any eliminated instr that is actually live (live wins).
    for (InstrId lid : result.liveInstrs) {
        result.eliminatedInstrs.erase(lid);
    }

    result.eliminatedInstrCount = result.eliminatedInstrs.size();

    return result;
}

} // namespace dce
} // namespace retdec
