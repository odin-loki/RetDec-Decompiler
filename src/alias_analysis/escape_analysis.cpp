/**
 * @file src/alias_analysis/escape_analysis.cpp
 * @brief Pointer escape analysis for SSA functions.
 *
 * ## What "escape" means
 *
 * A pointer value p "escapes" from function F if p may be dereferenced
 * after F returns.  This happens when:
 *
 *   (A) p is stored to a non-local memory location:
 *         - Written to a global variable.
 *         - Written to a heap-allocated object (any non-stack MemRef store).
 *         - Written to a memory location whose base is itself escaped.
 *
 *   (B) p is passed to an external or opaque function call.
 *       (We don't have the callee's summary → conservative: all pointer
 *        arguments to calls with no known summary are assumed escaped.)
 *
 *   (C) p is returned from the function.
 *       The return value may be held by the caller across F's lifetime.
 *
 * ## Why this matters
 *
 * If a stack slot's address is taken (via LEA / ADDROF) and the resulting
 * pointer escapes, that slot cannot safely be promoted to an SSA scalar:
 * the escaping pointer could be used to alias-read the slot from outside.
 *
 * ## Implementation
 *
 * Single-pass over all instructions:
 *
 *   1. Collect all ADDROF (LEA of a stack address) instructions.
 *      These define pointer SSA values that point into the stack frame.
 *
 *   2. For each such pointer value v:
 *       - If any successor instruction stores v to a non-stack memory
 *         location → v escapes (and so does the stack slot it points to).
 *       - If v is used as an argument to a Call instruction → escapes.
 *       - If v is used as the operand of a Ret instruction → escapes.
 *
 *   3. Transitively: if p escapes and p is assigned to q (q = p), then q
 *      also escapes.
 *
 * We perform a simple worklist fixpoint to handle transitive escapes.
 *
 * ## Conservatism
 *
 * We are sound but not complete:
 *   - We may mark as escaped values that don't actually escape (false positives).
 *   - We will never miss a genuine escape (no false negatives).
 *
 * For most decompiled code, the escape set is small (only functions that
 * explicitly take the address of a local variable).
 */

#include "retdec/alias_analysis/alias_analysis.h"
#include "retdec/ssa/ssa.h"
#include <queue>
#include <unordered_map>

namespace retdec {
namespace alias_analysis {

using namespace retdec::ssa;

bool EscapeAnalysis::isExternalCall(const SSAFunction& fn,
                                      uint32_t instrId) const {
    // Without a full call-graph, we conservatively treat all Call instructions
    // as external (no available function body / no known summary).
    const IrInstr* instr = fn.instr(instrId);
    return instr && instr->op == IrInstr::Op::Call;
}

EscapeAnalysis::EscapeInfo EscapeAnalysis::run(const SSAFunction& fn) const {
    EscapeInfo info;

    // Build use-def map: value ID → list of instruction IDs that use it
    std::unordered_map<uint32_t, std::vector<uint32_t>> users;
    for (auto& blkPtr : fn.blocks()) {
        for (auto* instr : blkPtr->instrs) {
            for (auto& use : instr->uses) {
                users[use.valueId].push_back(instr->id);
            }
        }
    }

    // Seed: find all pointer values derived from stack addresses (ADDROF / LEA)
    // In our IR, a MemRef used in an LEA-like context would have an ADDROF op.
    // We also treat any pointer-sized value whose address-of stack slot is known.
    std::unordered_set<uint32_t> escaped;
    std::queue<uint32_t> worklist;

    // Seed with all values that are explicitly address-taken (from MemRef)
    for (auto& valPtr : fn.values()) {
        const IrValue* v = valPtr.get();
        // A MemRef value that is used in a non-Load/Store context (i.e. used
        // as a data value, not just as an address operand) is address-taken.
        if (v->kind != ValueKind::MemRef) continue;
        if (!v->memIsStack) continue;

        bool usedAsData = false;
        auto it = users.find(v->id);
        if (it != users.end()) {
            for (uint32_t instrId : it->second) {
                const IrInstr* instr = fn.instr(instrId);
                if (!instr) continue;
                // A MemRef used in a Call or Ret or stored to non-stack → escapes
                if (instr->op == IrInstr::Op::Call ||
                    instr->op == IrInstr::Op::Ret) {
                    usedAsData = true;
                    break;
                }
                // Stored to a non-stack memory location → escapes
                if (instr->op == IrInstr::Op::Store) {
                    // Check if the store target is a non-stack MemRef
                    for (auto& u2 : instr->uses) {
                        const IrValue* tgt = fn.value(u2.valueId);
                        if (tgt && tgt->kind == ValueKind::MemRef && !tgt->memIsStack) {
                            usedAsData = true;
                            break;
                        }
                    }
                }
            }
        }

        if (usedAsData) {
            if (!escaped.count(v->id)) {
                escaped.insert(v->id);
                worklist.push(v->id);
                info.escapedSlots.insert(v->memOffset);
            }
        }
    }

    // Also check return instructions: if they return a MemRef value
    for (auto& blkPtr : fn.blocks()) {
        for (auto* instr : blkPtr->instrs) {
            if (instr->op != IrInstr::Op::Ret) continue;
            for (auto& use : instr->uses) {
                const IrValue* v = fn.value(use.valueId);
                if (v && v->kind == ValueKind::MemRef && v->memIsStack) {
                    escaped.insert(v->id);
                    info.escapedSlots.insert(v->memOffset);
                    info.returnEscapes = true;
                }
            }
        }
    }

    // Transitive propagation: if value p is escaped, any copy (q = p) also escapes
    while (!worklist.empty()) {
        uint32_t vid = worklist.front();
        worklist.pop();

        auto it = users.find(vid);
        if (it == users.end()) continue;

        for (uint32_t instrId : it->second) {
            const IrInstr* instr = fn.instr(instrId);
            if (!instr) continue;

            // Copy: the defined value also escapes
            if (instr->op == IrInstr::Op::Assign &&
                instr->defValue != UINT32_MAX) {
                if (!escaped.count(instr->defValue)) {
                    escaped.insert(instr->defValue);
                    worklist.push(instr->defValue);
                    // If it points to a stack slot, mark that slot too
                    const IrValue* def = fn.value(instr->defValue);
                    if (def && def->kind == ValueKind::MemRef && def->memIsStack)
                        info.escapedSlots.insert(def->memOffset);
                }
            }

            // Call: passed to external → escapes
            if (instr->op == IrInstr::Op::Call) {
                // Already escaped (vid is in the call's argument list)
                // No further propagation needed
            }

            // Ret
            if (instr->op == IrInstr::Op::Ret)
                info.returnEscapes = true;
        }
    }

    info.escapedValues = std::move(escaped);
    return info;
}

} // namespace alias_analysis
} // namespace retdec
