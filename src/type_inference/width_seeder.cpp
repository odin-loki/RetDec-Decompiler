/**
 * @file src/type_inference/width_seeder.cpp
 * @brief Phase 1: instruction-width extraction for SSA values.
 *
 * ## Width extraction rules
 *
 * An IrValue's bit-width is determined from its defining instruction's
 * operand size.  We use the IrValue::width field (populated by the decoder)
 * as the primary source, and fall back to inference from the instruction
 * opcode category:
 *
 *   VirtualReg  — use IrValue::width directly (set by decoder).
 *   FlagBundle  — always 1-bit (boolean per flag).
 *   MemRef      — use memWidth × 8 (bytes → bits).
 *   Phi         — resolve to the widest incoming operand (after seeding).
 *   Undef       — leave as 0 (unknown).
 *
 * For the defining instruction's opcode:
 *
 *   Assign / Mov        — width = defValue.width (direct register width).
 *   Compare             — result is a 1-bit boolean flag bundle.
 *   CondBranch / Branch — no output value.
 *   Load                — width = MemRef.memWidth × 8.
 *   Store               — no output value (writes to MemRef).
 *   Add / Sub / Mul     — width = max(operand widths).
 *   And / Or / Xor      — same as arithmetic.
 *   Shl / Shr / Sar     — width of the shifted value (lhs).
 *   Not / Neg           — width = operand width.
 *   Call                — width = ABI return register width (default 64).
 *   Ret                 — no output.
 *
 * ## Conflict resolution
 *
 * If a value's width is constrained by multiple sources (e.g. defined as
 * 32-bit but used in a 64-bit context with zero-extension), we record
 * both observations and resolve:
 *
 *   - If all sources agree → use that width.
 *   - If definition is narrower than uses (zero-extend pattern) → keep
 *     definition width (the logical value is still the narrow one).
 *   - If definition is wider than all uses (high bits unused) → keep
 *     definition width (avoids narrowing incorrectly).
 *   - Conflict between incompatible non-zero widths → use definition width
 *     and record as conflict.
 *
 * ## XMM / YMM / ZMM detection
 *
 * Values with width == 128 are XMM (vector of 4×float or 2×double).
 * Width == 256 → YMM (AVX).  Width == 512 → ZMM (AVX-512).
 * The lane type and count are refined by StructRecovery or ABI seeding.
 */

#include "retdec/type_inference/type_inference.h"
#include "retdec/ssa/ssa.h"
#include <algorithm>

namespace retdec {
namespace type_inference {

using namespace retdec::ssa;

// ─── Width from IrValue directly ─────────────────────────────────────────────

uint16_t WidthSeeder::widthFromInstr(const SSAFunction& fn,
                                       uint32_t valueId) const {
    const IrValue* v = fn.value(valueId);
    if (!v) return 0;

    // MemRef: width comes from memWidth (bytes → bits), NOT from IrValue::width
    // which defaults to 64 and would mask the actual access size.
    if (v->kind == ValueKind::MemRef && v->memWidth > 0)
        return (uint16_t)(v->memWidth * 8);

    // FlagBundle: 1-bit per flag, regardless of IrValue::width default
    if (v->kind == ValueKind::FlagBundle) return 1;

    // Values defined by Load instructions derive their width from the MemRef
    // operand, not from IrValue::width (which has a 64-bit default).
    if (v->defInstr && v->defInstr->op == IrInstr::Op::Load) {
        for (auto& use : v->defInstr->uses) {
            const IrValue* mv = fn.value(use.valueId);
            if (mv && mv->kind == ValueKind::MemRef && mv->memWidth > 0)
                return (uint16_t)(mv->memWidth * 8);
        }
        return 0;
    }

    // Direct width on the value (set by the decoder / earlier passes)
    if (v->width > 0) return v->width;

    // Derive from defining instruction
    if (!v->defInstr) return 0;
    const IrInstr* ins = v->defInstr;

    switch (ins->op) {
    case IrInstr::Op::Load:
        // Width comes from the MemRef operand
        for (auto& use : ins->uses) {
            const IrValue* mv = fn.value(use.valueId);
            if (mv && mv->kind == ValueKind::MemRef && mv->memWidth > 0)
                return (uint16_t)(mv->memWidth * 8);
        }
        return 0;

    case IrInstr::Op::Compare:
        return 1;  // result is boolean / flag bundle

    case IrInstr::Op::Call:
        return 64;  // default ABI return register width

    case IrInstr::Op::Add:
    case IrInstr::Op::Sub:
    case IrInstr::Op::Mul:
    case IrInstr::Op::And:
    case IrInstr::Op::Or:
    case IrInstr::Op::Xor:
    case IrInstr::Op::Not:
    case IrInstr::Op::Neg:
    case IrInstr::Op::Shl:
    case IrInstr::Op::Shr:
    case IrInstr::Op::Sar:
    case IrInstr::Op::Ror:
    case IrInstr::Op::Rol:
        // Width = widest operand
        {
            uint16_t maxW = 0;
            for (auto& use : ins->uses) {
                const IrValue* u = fn.value(use.valueId);
                if (u && u->width > maxW) maxW = u->width;
            }
            return maxW;
        }

    case IrInstr::Op::Assign:
        // Width from the source operand
        if (!ins->uses.empty()) {
            const IrValue* src = fn.value(ins->uses[0].valueId);
            if (src && src->width > 0) return src->width;
        }
        return 0;

    default:
        return 0;
    }
}

// ─── Phi width resolution ─────────────────────────────────────────────────────

static uint16_t resolvePhiWidth(const SSAFunction& fn,
                                  const IrValue* phiVal,
                                  const std::unordered_map<uint32_t,uint16_t>& known) {
    if (!phiVal->defPhi) return 0;
    uint16_t maxW = 0;
    for (auto& [predId, incoming] : phiVal->defPhi->operands) {
        auto it = known.find(incoming);
        if (it != known.end() && it->second > maxW) maxW = it->second;
        else {
            const IrValue* iv = fn.value(incoming);
            if (iv && iv->width > maxW) maxW = iv->width;
        }
    }
    return maxW;
}

// ─── Main seeding pass ────────────────────────────────────────────────────────

WidthSeeder::Result WidthSeeder::run(const SSAFunction& fn) const {
    Result res;

    // First pass: direct extraction for non-phi values
    for (auto& valPtr : fn.values()) {
        uint32_t id = valPtr->id;
        if (valPtr->kind == ValueKind::Phi) continue;
        if (valPtr->kind == ValueKind::Undef) continue;

        uint16_t w = widthFromInstr(fn, id);
        if (w > 0) {
            res.widths[id] = w;
            ++res.seededCount;
        }
    }

    // Second pass: phi resolution using already-known widths
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& valPtr : fn.values()) {
            if (valPtr->kind != ValueKind::Phi) continue;
            if (res.widths.count(valPtr->id)) continue;

            uint16_t w = resolvePhiWidth(fn, valPtr.get(), res.widths);
            if (w > 0) {
                res.widths[valPtr->id] = w;
                ++res.seededCount;
                changed = true;
            }
        }
    }

    // Conflict detection: count values where use context implies a different width
    for (auto& blkPtr : fn.blocks()) {
        for (auto* instr : blkPtr->instrs) {
            if (instr->defValue == UINT32_MAX) continue;
            auto it = res.widths.find(instr->defValue);
            if (it == res.widths.end()) continue;

            for (auto& use : instr->uses) {
                auto iu = res.widths.find(use.valueId);
                if (iu == res.widths.end()) continue;
                if (iu->second != it->second &&
                    iu->second > 0 && it->second > 0) {
                    ++res.conflictCount;
                    // Resolution: keep definition width (authoritative)
                    break;
                }
            }
        }
    }

    return res;
}

} // namespace type_inference
} // namespace retdec
