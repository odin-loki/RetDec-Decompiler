/**
 * @file src/var_recovery/dvsa.cpp
 * @brief DVSA: Data-flow-driven Variable and Stack-slot Analysis.
 *
 * ## Algorithm overview
 *
 * ### Step 1: Collect accesses
 *
 * Walk every `MemRef` value in the SSAFunction.  A MemRef records:
 *   memBaseReg — the base register (RBP, RSP, SP, etc.)
 *   memOffset  — the signed offset from that register
 *   memWidth   — access size in bytes
 *   memIsStack — true if this is a stack frame access
 *
 * We only analyse stack-relative accesses (`memIsStack == true`).  Heap
 * accesses are handled separately by alias analysis (Stage 18).
 *
 * All offsets are normalised to be relative to the frame base (RBP/EBP/X29)
 * so that SP-relative and BP-relative accesses to the same slot are unified.
 *
 *   normalised_offset = access.offset - prologue.frameSize  (if SP-based)
 *   normalised_offset = access.offset                       (if BP-based)
 *
 * ABI-reserved regions are excluded here (isCarved filter).
 *
 * ### Step 2: Sort by offset
 *
 * Sort accesses by `offset` then by `size` (descending, so wider accesses
 * come first and act as the slot anchor).
 *
 * ### Step 3: Non-overlap partitioning
 *
 * Sweep through the sorted list.  Maintain a "current slot" covering
 * [slot_start, slot_end).  Extend the slot whenever the next access
 * overlaps the current range; start a new slot when it doesn't.
 *
 *   slot_start = first access's offset
 *   slot_end   = slot_start + first access's size
 *   for each subsequent access a:
 *     if a.offset >= slot_end:           // no overlap
 *       emit current slot; start new slot at a
 *     elif a.offset + a.size <= slot_end: // fully contained
 *       record as sub-access of current slot
 *     else:                              // partial overlap
 *       extend slot_end = a.offset + a.size
 *
 * ### Step 4: Overlap classification
 *
 * For slots that contain multiple accesses of different sizes (sub-access or
 * extension overlaps), check whether their live ranges overlap:
 *
 *   If live ranges don't overlap → variable reuse: split into separate
 *   FrameSlots with different SSA value chains.
 *
 *   If live ranges do overlap → union: keep as a single FrameSlot with
 *   `isUnion=true`.  Code generation will emit a C anonymous union.
 *
 * ## Normalisation details
 *
 * To keep the implementation simple, we always normalise to RBP-relative
 * offsets:
 *   - If the access uses RBP directly, offset is already normalised.
 *   - If the access uses RSP, add `frameSize` to convert:
 *     rbp_offset = rsp_offset - frameSize
 *     (because RSP = RBP - frameSize at function start)
 *
 * This means all offsets in the output are negative for local variables
 * and positive for arguments (above RBP).
 */

#include "retdec/var_recovery/var_recovery.h"
#include "retdec/ssa/ssa.h"
#include <algorithm>
#include <cassert>

namespace retdec {
namespace var_recovery {

// ─── Carved region check ─────────────────────────────────────────────────────

bool DVSA::isCarved(const PrologueInfo& info,
                     int64_t off, uint8_t sz) const {
    // Check if [off, off+sz) intersects any ABI region
    for (auto& r : info.abiRegions) {
        int64_t aEnd = off + (int64_t)sz;
        int64_t rEnd = r.offset + (int64_t)r.size;
        if (off < rEnd && r.offset < aEnd) return true;
    }
    return false;
}

// ─── Access collection ────────────────────────────────────────────────────────

// SP-relative to RBP-relative normalisation:
//   rbp_off = sp_off - frame_size
//   For local vars: sp_off is negative (below RSP), rbp_off also negative.
//   For args: sp_off is frame_size + N, rbp_off = N > 0.
static bool isBaseRegSP(ssa::VarId reg) {
    // RSP = Reg 4 in x86-64 enum, ESP = 4+20 in x86-32, SP_ARM64 = 80, SP_ARM32
    // Since we're checking the varId which maps to Reg enum values:
    return reg == (ssa::VarId)var_recovery::Reg::RSP  ||
           reg == (ssa::VarId)var_recovery::Reg::ESP  ||
           reg == (ssa::VarId)var_recovery::Reg::SP_ARM64 ||
           reg == (ssa::VarId)var_recovery::Reg::SP_ARM32;
}

std::vector<FrameAccess> DVSA::collectAccesses(
    const ssa::SSAFunction& fn,
    const PrologueInfo& prologue) const {

    std::vector<FrameAccess> result;

    for (auto& valPtr : fn.values()) {
        const ssa::IrValue* v = valPtr.get();
        if (v->kind != ssa::ValueKind::MemRef) continue;
        if (!v->memIsStack) continue;

        // Normalise offset to RBP-relative
        int64_t off = v->memOffset;
        if (isBaseRegSP(v->memBaseReg)) {
            // Convert SP-relative → RBP-relative
            off = v->memOffset - (int64_t)prologue.frameSize;
        }

        // Exclude ABI-reserved regions
        if (isCarved(prologue, off, v->memWidth)) continue;

        FrameAccess acc;
        acc.offset   = off;
        acc.size     = v->memWidth;
        acc.ssaValue = v->id;
        // isWrite is determined from the defining instruction's op
        if (v->defInstr) {
            acc.isWrite = (v->defInstr->op == ssa::IrInstr::Op::Store);
            acc.vma     = v->defInstr->vma;
        }
        result.push_back(acc);
    }

    // Sort by offset, then by size descending (wider = anchor)
    std::sort(result.begin(), result.end(),
              [](const FrameAccess& a, const FrameAccess& b) {
                  if (a.offset != b.offset) return a.offset < b.offset;
                  return a.size > b.size;  // wider first
              });

    return result;
}

// ─── Non-overlap partitioning ─────────────────────────────────────────────────

std::vector<FrameSlot> DVSA::partition(
    std::vector<FrameAccess>& accesses) const {

    std::vector<FrameSlot> slots;
    if (accesses.empty()) return slots;

    FrameSlot current;
    current.baseOffset = accesses[0].offset;
    current.totalSize  = accesses[0].size;
    current.maxAccess  = accesses[0].size;
    current.hasWrite   = accesses[0].isWrite;
    current.hasRead    = !accesses[0].isWrite;
    current.accesses.push_back(accesses[0]);

    for (std::size_t i = 1; i < accesses.size(); ++i) {
        const FrameAccess& acc = accesses[i];
        int64_t slotEnd = current.baseOffset + (int64_t)current.totalSize;

        if (acc.offset >= slotEnd) {
            // No overlap → emit current slot, start new
            slots.push_back(std::move(current));
            current = FrameSlot{};
            current.baseOffset = acc.offset;
            current.totalSize  = acc.size;
            current.maxAccess  = acc.size;
        } else {
            // Overlap or sub-access → extend if necessary
            int64_t newEnd = acc.offset + (int64_t)acc.size;
            if (newEnd > slotEnd)
                current.totalSize = (uint8_t)(newEnd - current.baseOffset);
            if (acc.size > current.maxAccess)
                current.maxAccess = acc.size;
        }

        current.hasWrite |= acc.isWrite;
        current.hasRead  |= !acc.isWrite;
        current.accesses.push_back(acc);
    }
    slots.push_back(std::move(current));

    return slots;
}

// ─── Overlap check (for union detection) ─────────────────────────────────────

bool DVSA::accessesOverlap(const FrameAccess& a, const FrameAccess& b) const {
    int64_t aEnd = a.offset + (int64_t)a.size;
    int64_t bEnd = b.offset + (int64_t)b.size;
    return a.offset < bEnd && b.offset < aEnd;
}

// ─── Main DVSA run ────────────────────────────────────────────────────────────

DVSA::Result DVSA::run(const ssa::SSAFunction& fn,
                         const PrologueInfo& prologue) const {
    Result res;

    auto allAccesses = collectAccesses(fn, prologue);
    res.totalAccesses = allAccesses.size();

    auto slots = partition(allAccesses);

    // Classify each slot: single-access → normal, multi-access → check for union
    for (auto& slot : slots) {
        if (slot.accesses.size() <= 1) {
            res.slots.push_back(slot);
            continue;
        }

        // Check if any pair of sub-accesses has overlapping byte ranges
        bool hasOverlap = false;
        for (std::size_t i = 0; i < slot.accesses.size() && !hasOverlap; ++i)
            for (std::size_t j = i + 1; j < slot.accesses.size(); ++j)
                if (accessesOverlap(slot.accesses[i], slot.accesses[j])) {
                    hasOverlap = true;
                    break;
                }

        if (hasOverlap) {
            // Union slot: overlapping byte ranges → must keep as union
            // (Liveness check would require the full SSA liveness info here;
            //  we conservatively emit union for any byte-level overlap.)
            res.unionSlots.push_back(slot);
        } else {
            // Sub-accesses are at different, non-overlapping byte ranges
            // within the total slot span.  Treat each as a separate variable.
            for (auto& acc : slot.accesses) {
                FrameSlot sub;
                sub.baseOffset = acc.offset;
                sub.totalSize  = acc.size;
                sub.maxAccess  = acc.size;
                sub.hasWrite   = acc.isWrite;
                sub.hasRead    = !acc.isWrite;
                sub.accesses   = {acc};
                res.slots.push_back(sub);
            }
        }
    }

    // Re-sort final slots by baseOffset
    std::sort(res.slots.begin(), res.slots.end(),
              [](const FrameSlot& a, const FrameSlot& b) {
                  return a.baseOffset < b.baseOffset;
              });

    return res;
}

} // namespace var_recovery
} // namespace retdec
