/**
 * @file src/jvm_reconstruct/pattern_lift.cpp
 * @brief String concatenation, lambda, and enhanced for-loop detection.
 */

#include "retdec/jvm_reconstruct/pattern_lift.h"

#include <algorithm>

namespace retdec {
namespace jvm_reconstruct {

using namespace bc_module;

// ─── Helper: check method owner/name ─────────────────────────────────────────

static bool methodIs(const BcInstruction& insn,
                      const std::string& owner, const std::string& name) {
    if (insn.operands.empty()) return false;
    if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
        return m->owner == owner && m->name == name;
    return false;
}

// ─── String concatenation ────────────────────────────────────────────────────

bool PatternLifter::isStringConcatInvokeDynamic(const BcInstruction& insn) {
    if (insn.opcode != BcOpcode::InvokeDynamic) return false;
    if (insn.operands.empty()) return false;
    if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
        return m->name == "makeConcatWithConstants" ||
               m->name == "makeConcat";
    return false;
}

void PatternLifter::detectStringConcat(const BcCFG& cfg,
                                        const StackSimResult& simResult,
                                        PatternLiftResult& result) const {
    // Detect Java 9+ invokedynamic string concat.
    for (const auto& blk : cfg.blocks()) {
        for (size_t i = 0; i < blk.instrs.size(); ++i) {
            const auto& insn = blk.instrs[i];
            if (!isStringConcatInvokeDynamic(insn)) continue;

            StringConcatPattern pat;
            pat.blockId     = blk.id;
            pat.firstInstrId= insn.id;
            pat.lastInstrId = insn.id;

            // Collect the pop slots (concatenation parts).
            auto it = simResult.instrInfo.find(insn.id);
            if (it != simResult.instrInfo.end())
                pat.partSlotIds = it->second.pops;

            result.stringConcats.push_back(std::move(pat));
        }
    }

    // Detect Java 8 StringBuilder chain:
    //   new StringBuilder → append* → toString
    for (const auto& blk : cfg.blocks()) {
        for (size_t i = 0; i < blk.instrs.size(); ++i) {
            const auto& insn = blk.instrs[i];
            if (insn.opcode != BcOpcode::New) continue;

            // Check if allocating StringBuilder.
            bool isSB = false;
            if (!insn.operands.empty()) {
                if (auto* t = std::get_if<BcTypeOperand>(&insn.operands[0])) {
                    if (t->type.isRef()) {
                        const std::string& cn = t->type.ref().className;
                        isSB = (cn == "java.lang.StringBuilder" ||
                                cn == "java/lang/StringBuilder");
                    }
                }
            }
            if (!isSB) continue;

            // Walk forward collecting append calls until toString.
            StringConcatPattern pat;
            pat.blockId     = blk.id;
            pat.firstInstrId= insn.id;
            pat.lastInstrId = insn.id;

            for (size_t j = i + 1; j < blk.instrs.size(); ++j) {
                const auto& ni = blk.instrs[j];
                if (methodIs(ni, "java.lang.StringBuilder", "append") ||
                    methodIs(ni, "java/lang/StringBuilder",  "append")) {
                    auto it = simResult.instrInfo.find(ni.id);
                    if (it != simResult.instrInfo.end()) {
                        // The second pop is the part being appended.
                        if (it->second.pops.size() >= 2)
                            pat.partSlotIds.push_back(it->second.pops[1]);
                    }
                    pat.lastInstrId = ni.id;
                } else if (methodIs(ni, "java.lang.StringBuilder", "toString") ||
                           methodIs(ni, "java/lang/StringBuilder",  "toString")) {
                    pat.lastInstrId = ni.id;
                    result.stringConcats.push_back(std::move(pat));
                    break;
                } else {
                    break; // chain broken
                }
            }
        }
    }
}

// ─── Lambda detection ─────────────────────────────────────────────────────────

bool PatternLifter::isLambdaInvokeDynamic(const BcInstruction& insn,
                                            LambdaPattern& out) {
    if (insn.opcode != BcOpcode::InvokeDynamic) return false;
    if (insn.operands.empty()) return false;
    auto* m = std::get_if<BcMethodRef>(&insn.operands[0]);
    if (!m) return false;

    // LambdaMetafactory bootstrap method names.
    bool isMeta = (m->owner == "java/lang/invoke/LambdaMetafactory" ||
                   m->owner == "java.lang.invoke.LambdaMetafactory");
    if (!isMeta) return false;

    out.functionalInterface = (m->descriptor.returnType && m->descriptor.returnType->isRef())
                              ? m->descriptor.returnType->ref().className
                              : "";

    // The implementation method is in operands[1] (if present).
    if (insn.operands.size() > 1) {
        if (auto* impl = std::get_if<BcMethodRef>(&insn.operands[1])) {
            out.implMethod = *impl;
            // If the impl method is a lambda$… synthetic, it's a lambda body.
            // Otherwise it's a method reference.
            out.kind = (impl->name.find("lambda$") == 0)
                       ? LambdaKind::Lambda
                       : LambdaKind::MethodReference;
        }
    }
    return true;
}

void PatternLifter::detectLambda(const BcCFG& cfg,
                                  const BcMethod& /*method*/,
                                  const StackSimResult& simResult,
                                  PatternLiftResult& result) const {
    for (const auto& blk : cfg.blocks()) {
        for (const auto& insn : blk.instrs) {
            LambdaPattern pat;
            pat.blockId = blk.id;
            pat.instrId = insn.id;
            if (!isLambdaInvokeDynamic(insn, pat)) continue;

            auto it = simResult.instrInfo.find(insn.id);
            if (it != simResult.instrInfo.end())
                pat.captureSlots = it->second.pops;

            result.lambdas.push_back(std::move(pat));
        }
    }
}

// ─── Enhanced for-loop: Iterator pattern ─────────────────────────────────────

std::optional<ForEachPattern>
PatternLifter::detectIteratorLoop(const BcCFG& cfg,
                                   uint32_t loopHeaderBlock,
                                   const StackSimResult& simResult) {
    if (loopHeaderBlock >= cfg.blockCount()) return {};
    const BcBasicBlock& hdr = cfg.block(loopHeaderBlock);

    // Look for: invokeinterface Iterator.hasNext / invokeinterface hasNext
    bool hasHasNext = false;
    uint32_t iteratorSlot = 0;
    for (const auto& insn : hdr.instrs) {
        if (insn.opcode == BcOpcode::InvokeInterface ||
            insn.opcode == BcOpcode::InvokeVirtual) {
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0])) {
                    if (m->name == "hasNext") {
                        hasHasNext = true;
                        // The instance is on the stack.
                        auto it = simResult.instrInfo.find(insn.id);
                        if (it != simResult.instrInfo.end() && !it->second.pops.empty())
                            iteratorSlot = it->second.pops.front();
                    }
                }
            }
        }
    }
    if (!hasHasNext) return {};

    // The loop body should have an invokeinterface Iterator.next().
    // Find the body block (first successor of header that's not the exit).
    if (hdr.succs.size() < 2) return {};
    uint32_t bodyBlock = hdr.succs[0];
    uint32_t exitBlock = hdr.succs[1];

    // Look for Iterator.next() call in the body.
    if (bodyBlock >= cfg.blockCount()) return {};
    const BcBasicBlock& body = cfg.block(bodyBlock);
    uint32_t elementSlot = 0;
    BcType elementType;
    for (const auto& insn : body.instrs) {
        if ((insn.opcode == BcOpcode::InvokeInterface ||
             insn.opcode == BcOpcode::InvokeVirtual) &&
            !insn.operands.empty()) {
            if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0])) {
                if (m->name == "next") {
                    auto it = simResult.instrInfo.find(insn.id);
                    if (it != simResult.instrInfo.end() && !it->second.pushes.empty()) {
                        elementSlot = it->second.pushes.front();
                        if (const StackSlot* s = simResult.slot(elementSlot))
                            elementType = s->type;
                    }
                }
            }
        }
    }

    ForEachPattern pat;
    pat.kind             = ForEachKind::Iterator;
    pat.loopHeaderBlock  = loopHeaderBlock;
    pat.bodyBlock        = bodyBlock;
    pat.exitBlock        = exitBlock;
    pat.elementSlot      = elementSlot;
    pat.elementType      = elementType;
    pat.collectionSlot   = 0;
    pat.iteratorSlot     = iteratorSlot;
    pat.indexSlot        = 0;
    pat.lengthSlot       = 0;
    return pat;
}

// ─── Enhanced for-loop: array pattern ────────────────────────────────────────

std::optional<ForEachPattern>
PatternLifter::detectArrayLoop(const BcCFG& cfg,
                                uint32_t loopHeaderBlock,
                                const StackSimResult& simResult) {
    if (loopHeaderBlock >= cfg.blockCount()) return {};
    const BcBasicBlock& hdr = cfg.block(loopHeaderBlock);

    // Pattern: load length local, load index, compare (if_icmpge → exit).
    bool hasLengthCheck = false;
    uint32_t indexSlot  = 0;
    uint32_t lengthSlot = 0;

    for (const auto& insn : hdr.instrs) {
        if (insn.opcode == BcOpcode::CmpLt || insn.opcode == BcOpcode::CmpGe ||
            insn.opcode == BcOpcode::IfLt  || insn.opcode == BcOpcode::IfGe) {
            auto it = simResult.instrInfo.find(insn.id);
            if (it != simResult.instrInfo.end() && it->second.pops.size() >= 2) {
                indexSlot  = it->second.pops[0];
                lengthSlot = it->second.pops[1];
                hasLengthCheck = true;
            }
        }
    }
    if (!hasLengthCheck) return {};

    // The body should contain an array load instruction.
    if (hdr.succs.size() < 2) return {};
    uint32_t bodyBlock = hdr.succs[0];
    uint32_t exitBlock = hdr.succs[1];
    if (bodyBlock >= cfg.blockCount()) return {};

    const BcBasicBlock& body = cfg.block(bodyBlock);
    uint32_t elementSlot = 0;
    BcType elementType;
    uint32_t arraySlot = 0;
    for (const auto& insn : body.instrs) {
        if (insn.opcode == BcOpcode::ArrayLoad) {
            auto it = simResult.instrInfo.find(insn.id);
            if (it != simResult.instrInfo.end()) {
                if (!it->second.pops.empty())
                    arraySlot = it->second.pops[0];
                if (!it->second.pushes.empty()) {
                    elementSlot = it->second.pushes.front();
                    if (const StackSlot* s = simResult.slot(elementSlot))
                        elementType = s->type;
                }
            }
        }
    }

    ForEachPattern pat;
    pat.kind            = ForEachKind::Array;
    pat.loopHeaderBlock = loopHeaderBlock;
    pat.bodyBlock       = bodyBlock;
    pat.exitBlock       = exitBlock;
    pat.elementSlot     = elementSlot;
    pat.elementType     = elementType;
    pat.collectionSlot  = arraySlot;
    pat.iteratorSlot    = 0;
    pat.indexSlot       = indexSlot;
    pat.lengthSlot      = lengthSlot;
    return pat;
}

// ─── For-each detection ───────────────────────────────────────────────────────

void PatternLifter::detectForEach(const BcCFG& cfg,
                                   const StackSimResult& simResult,
                                   const LocalRebuildResult& /*locals*/,
                                   PatternLiftResult& result) const {
    // Identify loop headers: blocks that are their own predecessors
    // (back-edge target) or have multiple predecessors.
    for (const auto& blk : cfg.blocks()) {
        bool isLoopHeader = blk.isLoopHeader;
        // Also detect via back-edge: if any successor has an earlier id.
        if (!isLoopHeader) {
            for (uint32_t succ : blk.succs) {
                // Simple heuristic: if a successor's id ≤ this block's id,
                // the target is a loop header.
                if (succ <= blk.id && succ != blk.id) {
                    // The target is a loop header.
                    if (auto pat = detectIteratorLoop(cfg, succ, simResult))
                        result.forEachLoops.push_back(*pat);
                    else if (auto pat = detectArrayLoop(cfg, succ, simResult))
                        result.forEachLoops.push_back(*pat);
                    break;
                }
            }
        } else {
            if (auto pat = detectIteratorLoop(cfg, blk.id, simResult))
                result.forEachLoops.push_back(*pat);
            else if (auto pat = detectArrayLoop(cfg, blk.id, simResult))
                result.forEachLoops.push_back(*pat);
        }
    }

    // Deduplicate (same loopHeaderBlock may be detected twice).
    std::sort(result.forEachLoops.begin(), result.forEachLoops.end(),
              [](const ForEachPattern& a, const ForEachPattern& b) {
                  return a.loopHeaderBlock < b.loopHeaderBlock;
              });
    result.forEachLoops.erase(
        std::unique(result.forEachLoops.begin(), result.forEachLoops.end(),
                    [](const ForEachPattern& a, const ForEachPattern& b) {
                        return a.loopHeaderBlock == b.loopHeaderBlock;
                    }),
        result.forEachLoops.end());
}

// ─── PatternLifter main ───────────────────────────────────────────────────────

PatternLiftResult PatternLifter::lift(const BcCFG& cfg,
                                       const BcMethod& method,
                                       const StackSimResult& simResult,
                                       const LocalRebuildResult& locals) {
    PatternLiftResult result;
    detectStringConcat(cfg, simResult, result);
    detectLambda(cfg, method, simResult, result);
    detectForEach(cfg, simResult, locals, result);
    return result;
}

} // namespace jvm_reconstruct
} // namespace retdec
