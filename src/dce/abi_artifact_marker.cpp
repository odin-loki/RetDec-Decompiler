/**
 * @file src/dce/abi_artifact_marker.cpp
 * @brief ABI artifact detection: stack alignment, prologue/epilogue,
 *        shadow space, callee-save pairs, red zone.
 */

#include "retdec/dce/dce.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <unordered_set>
#include <queue>

namespace retdec {
namespace dce {

// ─── Helpers ─────────────────────────────────────────────────────────────────

// True if the instruction is AND with an immediate that is a negative
// power-of-two (stack alignment pattern: AND RSP, -16 / AND RSP, -32).
bool AbiArtifactMarker::isRspAlignInstr(const ssa::SSAFunction& fn,
                                          InstrId id) const {
    const ssa::IrInstr* instr = fn.instr(id);
    if (!instr || instr->op != ssa::IrInstr::Op::And) return false;

    // Check for an immediate operand that is a negative power-of-two.
    for (const auto& use : instr->uses) {
        const ssa::IrValue* val = fn.value(use.valueId);
        if (!val) continue;
        if (val->kind == ssa::ValueKind::Immediate) {
            int64_t imm = static_cast<int64_t>(val->imm);
            // Negative and power-of-two in magnitude: -16, -32, -64
            if (imm < 0) {
                int64_t mag = -imm;
                if ((mag & (mag - 1)) == 0 && mag <= 64) return true;
            }
        }
    }
    return false;
}

bool AbiArtifactMarker::isPrologueBlock(BlockId blk,
                                          const ssa::SSAFunction& fn) const {
    return blk == fn.entryId();
}

bool AbiArtifactMarker::isCalleeSaveReg(const std::string& name,
                                          bool win64) const {
    // SysV AMD64 callee-saved: rbx, rbp, r12, r13, r14, r15
    static const char* sysv[] = {"rbx","rbp","r12","r13","r14","r15", nullptr};
    // Win64 adds: rdi, rsi, xmm6..xmm15 (we handle GP regs here)
    static const char* win64extra[] = {"rdi","rsi", nullptr};

    for (int i = 0; sysv[i]; ++i)
        if (name == sysv[i]) return true;

    if (win64)
        for (int i = 0; win64extra[i]; ++i)
            if (name == win64extra[i]) return true;

    return false;
}

// ─── Stack alignment ──────────────────────────────────────────────────────────

std::vector<AbiArtifact>
AbiArtifactMarker::markStackAlign(const ssa::SSAFunction& fn) const {
    std::vector<AbiArtifact> result;
    const ssa::BasicBlock* entry = fn.block(fn.entryId());
    if (!entry) return result;

    // Scan the first 8 instructions of the entry block.
    const std::size_t limit = std::min(entry->instrs.size(), std::size_t(8));
    for (std::size_t i = 0; i < limit; ++i) {
        const ssa::IrInstr* instr = entry->instrs[i];
        if (!instr) continue;
        if (isRspAlignInstr(fn, instr->id)) {
            AbiArtifact art;
            art.instrId = instr->id;
            art.kind    = AbiArtifactKind::StackAlign;
            art.balanced = true;
            result.push_back(art);
        }
    }
    return result;
}

// ─── Prologue / epilogue ──────────────────────────────────────────────────────

std::vector<AbiArtifact>
AbiArtifactMarker::markPrologueEpilogue(const ssa::SSAFunction& fn) const {
    std::vector<AbiArtifact> result;

    // Prologue: SUB RSP, N in entry block.
    const ssa::BasicBlock* entry = fn.block(fn.entryId());
    if (entry) {
        for (const ssa::IrInstr* instr : entry->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Sub) {
                // Check if the destination involves RSP.
                // Heuristic: if any use is an Immediate and this is the
                // entry block, it's likely SUB RSP, N.
                bool hasImm = false;
                for (const auto& use : instr->uses) {
                    const ssa::IrValue* v = fn.value(use.valueId);
                    if (v && v->kind == ssa::ValueKind::Immediate) {
                        hasImm = true; break;
                    }
                }
                if (hasImm) {
                    AbiArtifact art;
                    art.instrId = instr->id;
                    art.kind    = AbiArtifactKind::PrologueSetup;
                    result.push_back(art);
                    break;  // only first SUB in entry
                }
            }
        }
    }

    // Epilogue: ADD RSP, N immediately before RET blocks.
    for (const auto& blk : fn.blocks()) {
        if (!blk || blk->instrs.empty()) continue;
        const ssa::IrInstr* last = blk->instrs.back();
        if (!last || last->op != ssa::IrInstr::Op::Ret) continue;

        // Look backwards for ADD or similar.
        for (int i = static_cast<int>(blk->instrs.size()) - 2; i >= 0; --i) {
            const ssa::IrInstr* instr = blk->instrs[static_cast<std::size_t>(i)];
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Add) {
                bool hasImm = false;
                for (const auto& use : instr->uses) {
                    const ssa::IrValue* v = fn.value(use.valueId);
                    if (v && v->kind == ssa::ValueKind::Immediate) {
                        hasImm = true; break;
                    }
                }
                if (hasImm) {
                    AbiArtifact art;
                    art.instrId = instr->id;
                    art.kind    = AbiArtifactKind::EpilogueCleanup;
                    result.push_back(art);
                }
                break;
            }
            if (instr->op != ssa::IrInstr::Op::Store &&
                instr->op != ssa::IrInstr::Op::Load) break;
        }
    }

    return result;
}

// ─── Shadow space (Win64) ─────────────────────────────────────────────────────

std::vector<AbiArtifact>
AbiArtifactMarker::markShadowSpace(const ssa::SSAFunction& fn) const {
    std::vector<AbiArtifact> result;

    // Find all stack slot offsets that are written by this function.
    std::unordered_set<int64_t> writtenSlots;
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Store) continue;
            for (const auto& use : instr->uses) {
                const ssa::IrValue* v = fn.value(use.valueId);
                if (v && v->kind == ssa::ValueKind::MemRef && v->memIsStack) {
                    writtenSlots.insert(v->memOffset);
                }
            }
        }
    }

    // Win64 shadow space: [RSP+8] through [RSP+40] (4 × 8-byte slots).
    // If this function loads from a shadow slot it never writes, mark as artifact.
    static const int64_t shadowOffsets[] = {8, 16, 24, 32};
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Load) continue;
            for (const auto& use : instr->uses) {
                const ssa::IrValue* v = fn.value(use.valueId);
                if (!v || v->kind != ssa::ValueKind::MemRef || !v->memIsStack) continue;
                for (int64_t off : shadowOffsets) {
                    if (v->memOffset == off && !writtenSlots.count(off)) {
                        AbiArtifact art;
                        art.instrId = instr->id;
                        art.kind    = AbiArtifactKind::ShadowSpaceRead;
                        result.push_back(art);
                        break;
                    }
                }
            }
        }
    }

    return result;
}

// ─── Callee-save pairs ────────────────────────────────────────────────────────

std::vector<AbiArtifact>
AbiArtifactMarker::markCalleeSavePairs(const ssa::SSAFunction& fn,
                                         const Config& cfg) const {
    std::vector<AbiArtifact> result;

    // Step 1: Find save instructions (Stores in the prologue whose source
    // variable name matches a callee-save register).
    struct SaveInfo {
        InstrId  storeId  = UINT32_MAX;
        int64_t  offset   = 0;
        VarId    regVar   = ssa::kInvalidVar;
        std::string regName;
    };
    std::vector<SaveInfo> saves;

    const ssa::BasicBlock* entry = fn.block(fn.entryId());
    if (!entry) return result;

    // Scan entry block for callee-save stores.
    for (const ssa::IrInstr* instr : entry->instrs) {
        if (!instr || instr->op != ssa::IrInstr::Op::Store) continue;
        // A Store has uses: [dest_addr, src_value]
        if (instr->uses.size() < 2) continue;

        const ssa::IrValue* dest = fn.value(instr->uses[0].valueId);
        const ssa::IrValue* src  = fn.value(instr->uses[1].valueId);
        if (!dest || !src) continue;
        if (dest->kind != ssa::ValueKind::MemRef || !dest->memIsStack) continue;

        // Check if src is a callee-save register.
        const std::string& srcName = fn.varName(src->varId);
        if (!isCalleeSaveReg(srcName, cfg.win64)) continue;

        SaveInfo si;
        si.storeId = instr->id;
        si.offset  = dest->memOffset;
        si.regVar  = src->varId;
        si.regName = srcName;
        saves.push_back(si);
    }

    // Step 2: For each save, find a matching restore (Load from same slot
    // into a variable of the same name).
    for (const SaveInfo& save : saves) {
        InstrId restoreId = UINT32_MAX;
        bool balanced = false;

        for (const auto& blk : fn.blocks()) {
            if (!blk) continue;
            // Only look in blocks that dominate all RET blocks (epilogue).
            // Simplified: look in blocks ending in RET or just before RET.
            bool isEpilogue = false;
            for (const ssa::IrInstr* last : blk->instrs) {
                if (last && last->op == ssa::IrInstr::Op::Ret) {
                    isEpilogue = true; break;
                }
            }
            if (!isEpilogue) continue;

            for (const ssa::IrInstr* instr : blk->instrs) {
                if (!instr || instr->op != ssa::IrInstr::Op::Load) continue;
                if (instr->uses.empty()) continue;

                const ssa::IrValue* src = fn.value(instr->uses[0].valueId);
                if (!src || src->kind != ssa::ValueKind::MemRef) continue;
                if (!src->memIsStack) continue;
                if (src->memOffset != save.offset) continue;

                // Found matching load from same stack slot.
                restoreId = instr->id;
                balanced  = true;
                break;
            }
            if (balanced) break;
        }

        AbiArtifact art;
        art.instrId  = save.storeId;
        art.pairedId = restoreId;
        art.kind     = AbiArtifactKind::CalleeSavePair;
        art.balanced = balanced;
        art.savedReg = save.regVar;
        result.push_back(art);
    }

    return result;
}

// ─── Red zone ─────────────────────────────────────────────────────────────────

std::vector<AbiArtifact>
AbiArtifactMarker::markRedZone(const ssa::SSAFunction& fn) const {
    std::vector<AbiArtifact> result;

    // Find stores to [RSP - N] for N ∈ [1, 128].
    // Check if the stored value is read again before any RET.
    // (Conservative: we only flag clearly dead red-zone stores.)
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Store) continue;
            if (instr->uses.empty()) continue;

            const ssa::IrValue* dest = fn.value(instr->uses[0].valueId);
            if (!dest || dest->kind != ssa::ValueKind::MemRef) continue;
            if (!dest->memIsStack) continue;

            // Red zone: negative offsets from RSP in [−128, −1].
            if (dest->memOffset >= -128 && dest->memOffset < 0) {
                // Check if this slot is read later in the function.
                bool isRead = false;
                for (const auto& rb : fn.blocks()) {
                    if (!rb) continue;
                    for (const ssa::IrInstr* ri : rb->instrs) {
                        if (!ri || ri->op != ssa::IrInstr::Op::Load) continue;
                        for (const auto& use : ri->uses) {
                            const ssa::IrValue* rv = fn.value(use.valueId);
                            if (rv && rv->kind == ssa::ValueKind::MemRef &&
                                rv->memIsStack &&
                                rv->memOffset == dest->memOffset) {
                                isRead = true;
                                break;
                            }
                        }
                        if (isRead) break;
                    }
                    if (isRead) break;
                }

                if (!isRead) {
                    AbiArtifact art;
                    art.instrId = instr->id;
                    art.kind    = AbiArtifactKind::RedZoneAccess;
                    result.push_back(art);
                }
            }
        }
    }

    return result;
}

// ─── AbiArtifactMarker::run ───────────────────────────────────────────────────

std::vector<AbiArtifact>
AbiArtifactMarker::run(const ssa::SSAFunction& fn,
                         const Config& cfg) const {
    std::vector<AbiArtifact> all;

    auto append = [&](std::vector<AbiArtifact> v) {
        for (auto& a : v) all.push_back(std::move(a));
    };

    append(markStackAlign(fn));
    append(markPrologueEpilogue(fn));
    append(markCalleeSavePairs(fn, cfg));

    if (cfg.win64)     append(markShadowSpace(fn));
    if (cfg.sysVAmd64) append(markRedZone(fn));

    return all;
}

} // namespace dce
} // namespace retdec
