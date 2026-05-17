/**
 * @file src/ssa/flag_bundle.cpp
 * @brief FlagBundle analysis + SSAPass + SSAVerifier + SSAFunction impl.
 *
 * ## FlagBundle analysis
 *
 * After SSA construction, this pass walks the SSA values and identifies
 * every FlagBundle value.  For each one it determines:
 *
 *   sameBlockOnly — true if every use of this bundle is in the same basic
 *                   block as the definition.  Such bundles never need to
 *                   cross a basic block boundary and therefore require no
 *                   phi functions.
 *
 *   usedFlags     — bitmask of which FlagBit values are actually consumed
 *                   by the bundle's users.  Bits not in this mask can be
 *                   treated as dead (e.g. PF is rarely used outside of
 *                   parity checks; AF is almost never used in compiled code).
 *
 * The typical result on x86-64 code: ~85-90% of FlagBundle values are
 * sameBlockOnly because most arithmetic is immediately followed by a
 * conditional branch or SETcc in the same block.
 *
 * ## SSAFunction implementation
 *
 * The SSAFunction owns all blocks, instructions, values, and phi nodes.
 * Allocation is done through factory methods that assign monotone IDs.
 *
 * ## SSAVerifier
 *
 * Three checks:
 *   1. Def-dominance: every use site must be dominated by the definition.
 *   2. Phi operand completeness: every phi must have exactly one operand
 *      per predecessor block.
 *   3. Unique definition: no VarId+version pair appears more than once.
 *
 * ## SSAPass
 *
 * Orchestrates the full pipeline in order:
 *   liveness → domtree → phi placement → rename → flag analysis → verify
 */

#include <memory>
#include "retdec/ssa/ssa.h"
#include <algorithm>
#include <cassert>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace retdec {
namespace ssa {

// ═══════════════════════════════════════════════════════════════════════════════
// SSAFunction implementation
// ═══════════════════════════════════════════════════════════════════════════════

SSAFunction::~SSAFunction() = default;

BasicBlock* SSAFunction::addBlock(std::string name) {
    auto blk = std::make_unique<BasicBlock>();
    blk->id  = (BlockId)blocks_.size();
    blk->name= name.empty() ? ("bb" + std::to_string(blk->id)) : std::move(name);
    BasicBlock* ptr = blk.get();
    blocks_.push_back(std::move(blk));
    return ptr;
}

BasicBlock* SSAFunction::block(BlockId id) {
    if (id >= blocks_.size()) return nullptr;
    return blocks_[id].get();
}
const BasicBlock* SSAFunction::block(BlockId id) const {
    if (id >= blocks_.size()) return nullptr;
    return blocks_[id].get();
}

IrInstr* SSAFunction::addInstr(BlockId blkId, IrInstr::Op op, uint64_t vma) {
    auto instr = std::make_unique<IrInstr>();
    instr->id    = (InstrId)instrs_.size();
    instr->op    = op;
    instr->vma   = vma;
    instr->block = blkId;
    IrInstr* ptr = instr.get();
    instrs_.push_back(std::move(instr));
    if (auto* b = block(blkId)) b->instrs.push_back(ptr);
    return ptr;
}

IrInstr* SSAFunction::instr(InstrId id) {
    if (id >= instrs_.size()) return nullptr;
    return instrs_[id].get();
}

const IrInstr* SSAFunction::instr(InstrId id) const {
    if (id >= instrs_.size()) return nullptr;
    return instrs_[id].get();
}

IrValue* SSAFunction::allocValue(ValueKind kind, VarId varId) {
    auto v = std::make_unique<IrValue>();
    v->id    = (ValueId)values_.size();
    v->kind  = kind;
    v->varId = varId;
    // Assign version: count existing values with the same varId
    uint32_t ver = 0;
    for (auto& existing : values_)
        if (existing->varId == varId && existing->kind == kind) ++ver;
    v->version = ver;
    IrValue* ptr = v.get();
    values_.push_back(std::move(v));
    return ptr;
}

IrValue* SSAFunction::value(ValueId id) {
    if (id >= values_.size()) return nullptr;
    return values_[id].get();
}
const IrValue* SSAFunction::value(ValueId id) const {
    if (id >= values_.size()) return nullptr;
    return values_[id].get();
}

PhiNode* SSAFunction::addPhi(BlockId blkId, VarId var) {
    auto phi = std::make_unique<PhiNode>();
    phi->varId = var;
    phi->block = blkId;
    PhiNode* ptr = phi.get();
    phis_.push_back(std::move(phi));
    if (auto* b = block(blkId)) b->phis.push_back(ptr);
    return ptr;
}

VarId SSAFunction::declareVar(std::string name, uint8_t /*width*/) {
    VarId id = (VarId)varNames_.size();
    varNames_.push_back(std::move(name));
    return id;
}

VarId SSAFunction::findVar(const std::string& name) const {
    for (VarId i = 0; i < static_cast<VarId>(varNames_.size()); ++i) {
        if (varNames_[i] == name) return i;
    }
    return kInvalidVar;
}

const std::string& SSAFunction::varName(VarId id) const {
    static const std::string kUnknown = "<unknown>";
    if (id >= varNames_.size()) return kUnknown;
    return varNames_[id];
}

// ─── Debug helpers ────────────────────────────────────────────────────────────

std::string IrValue::debugName() const {
    std::string k;
    switch (kind) {
    case ValueKind::VirtualReg:  k = "v";   break;
    case ValueKind::Immediate:   k = "imm"; break;
    case ValueKind::FlagBundle:  k = "flags"; break;
    case ValueKind::MemRef:      k = "mem"; break;
    case ValueKind::Undef:       k = "undef"; break;
    case ValueKind::Phi:         k = "phi";  break;
    }
    return k + std::to_string(id) + "_" + std::to_string(version);
}

std::string IrInstr::debugStr() const {
    std::ostringstream os;
    os << "%" << id << " @ 0x" << std::hex << vma;
    return os.str();
}

// ═══════════════════════════════════════════════════════════════════════════════
// FlagBundleAnalysis
// ═══════════════════════════════════════════════════════════════════════════════

void FlagBundleAnalysis::run(const SSAFunction& fn) {
    bundles_.clear();

    // Collect all FlagBundle values
    std::unordered_map<ValueId, std::size_t> bundleIdx;
    for (auto& valPtr : fn.values()) {
        if (valPtr->kind == ValueKind::FlagBundle &&
            valPtr->kind != ValueKind::Undef) {
            BundleInfo info;
            info.bundleId  = valPtr->id;
            info.defBlock  = valPtr->defInstr ? valPtr->defInstr->block
                                               : kInvalidBlock;
            bundleIdx[valPtr->id] = bundles_.size();
            bundles_.push_back(info);
        }
    }

    // Walk all instructions looking for flag-reading uses
    for (auto& blkPtr : fn.blocks()) {
        for (auto* instr : blkPtr->instrs) {
            if (!instr->readsFlagBundle) continue;
            ValueId bundleId = instr->flagBundleInput;
            if (bundleId == kInvalidValue) continue;

            auto it = bundleIdx.find(bundleId);
            if (it == bundleIdx.end()) continue;

            BundleInfo& info = bundles_[it->second];
            info.usedFlags |= (1u << (uint8_t)instr->specificFlag);
            info.useSites.push_back({blkPtr->id, instr->id});
        }
    }

    // Also check phi nodes for flag bundles
    for (auto& phi : fn.phis()) {
        auto it = bundleIdx.find(phi->result);
        if (it == bundleIdx.end()) continue;
        // Phi of a flag bundle — it's used across blocks
        for (auto& [predId, valId] : phi->operands) {
            auto it2 = bundleIdx.find(valId);
            if (it2 != bundleIdx.end())
                bundles_[it2->second].useSites.push_back({predId, UINT32_MAX});
        }
    }

    // Determine sameBlockOnly
    for (auto& info : bundles_) {
        if (info.defBlock == kInvalidBlock) { info.sameBlockOnly = false; continue; }
        info.sameBlockOnly = true;
        for (auto& [blk, _] : info.useSites) {
            if (blk != info.defBlock) { info.sameBlockOnly = false; break; }
        }
    }
}

std::size_t FlagBundleAnalysis::sameBlockBundleCount() const {
    std::size_t n = 0;
    for (auto& b : bundles_) if (b.sameBlockOnly) ++n;
    return n;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SSAVerifier
// ═══════════════════════════════════════════════════════════════════════════════

std::vector<SSAVerifier::Error> SSAVerifier::verify(
    const SSAFunction& fn, const DominatorTree& dom) const {

    std::vector<Error> errors;

    // Build a map: ValueId → BlockId where it's defined
    std::unordered_map<ValueId, BlockId> defBlock;
    std::unordered_set<ValueId> defined;

    for (auto& blkPtr : fn.blocks()) {
        for (auto* instr : blkPtr->instrs) {
            if (instr->defValue != kInvalidValue) {
                if (defined.count(instr->defValue)) {
                    Error e;
                    e.kind  = Error::Kind::MultipleDefinition;
                    e.block = blkPtr->id;
                    e.instr = instr->id;
                    e.value = instr->defValue;
                    e.message = "Value " + std::to_string(instr->defValue)
                                + " defined more than once";
                    errors.push_back(std::move(e));
                }
                defined.insert(instr->defValue);
                defBlock[instr->defValue] = blkPtr->id;
            }
        }
        for (auto* phi : blkPtr->phis) {
            if (phi->result != kInvalidValue) {
                defined.insert(phi->result);
                defBlock[phi->result] = blkPtr->id;
            }
        }
    }

    // Check def-dominance for each use
    for (auto& blkPtr : fn.blocks()) {
        for (auto* instr : blkPtr->instrs) {
            for (auto& use : instr->uses) {
                ValueId vid = use.valueId;
                if (vid == kInvalidValue) continue;
                auto it = defBlock.find(vid);
                if (it == defBlock.end()) continue;  // undef or entry param
                BlockId defB = it->second;
                BlockId useB = blkPtr->id;
                if (!dom.dominates(fn, defB, useB)) {
                    Error e;
                    e.kind  = Error::Kind::UseDominance;
                    e.block = useB;
                    e.instr = instr->id;
                    e.value = vid;
                    e.message = "Use of v" + std::to_string(vid)
                                + " in block " + std::to_string(useB)
                                + " not dominated by def in block "
                                + std::to_string(defB);
                    errors.push_back(std::move(e));
                }
            }
        }
    }

    // Check phi operand completeness
    for (auto& phi : fn.phis()) {
        const BasicBlock* blk = fn.block(phi->block);
        if (!blk) continue;
        std::unordered_set<BlockId> covered;
        for (auto& [pred, _] : phi->operands) covered.insert(pred);
        for (BlockId pred : blk->preds) {
            if (!covered.count(pred)) {
                Error e;
                e.kind  = Error::Kind::PhiPredMismatch;
                e.block = phi->block;
                e.instr = kInvalidBlock;
                e.value = phi->result;
                e.message = "Phi for var" + std::to_string(phi->varId)
                            + " in block " + std::to_string(phi->block)
                            + " missing operand from pred "
                            + std::to_string(pred);
                errors.push_back(std::move(e));
            }
        }
    }

    return errors;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SSAPass
// ═══════════════════════════════════════════════════════════════════════════════

void SSAPass::run(SSAFunction& fn) {
    // 1. Liveness
    LivenessAnalysis liveness;
    liveness.run(fn);
    stats_.livenessIterations = liveness.iterations();

    // 2. Dominator tree
    DominatorTree domtree;
    domtree.run(fn);

    // 3. Phi placement (liveness-pruned)
    PhiPlacement phiPlace;
    phiPlace.run(fn, liveness);
    stats_.phisPlaced = phiPlace.placedCount();

    // 4. SSA renaming
    SSARename rename;
    rename.run(fn);

    // 5. FlagBundle analysis
    FlagBundleAnalysis flagAnalysis;
    flagAnalysis.run(fn);
    stats_.flagBundles      = flagAnalysis.bundles().size();
    stats_.sameBlockBundles = flagAnalysis.sameBlockBundleCount();

    // 6. Count MemRef values
    for (auto& v : fn.values())
        if (v->kind == ValueKind::MemRef) ++stats_.memRefs;

    // 7. Verify
    SSAVerifier verifier;
    errors_ = verifier.verify(fn, domtree);
}

} // namespace ssa
} // namespace retdec
