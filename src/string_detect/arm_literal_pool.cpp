/**
 * @file src/string_detect/arm_literal_pool.cpp
 * @brief ARM32/Thumb literal pool extraction.
 *
 * ## ARM literal pools
 *
 * ARM32 and Thumb instruction sets lack a 32-bit immediate mode.  To load a
 * 32-bit constant (address or large integer), the compiler places the value
 * in a "literal pool" — a small block of data embedded in the .text section
 * immediately after the function body (or between basic blocks in Thumb).
 *
 * The instruction form is:
 *   LDR Rd, [PC, #offset]      (ARM32)
 *   LDR Rd, [PC, #offset]      (Thumb, 4-byte aligned)
 *
 * The platform adapter decodes these and emits InstrRef records where:
 *   instrVma    = address of the LDR instruction
 *   targetVma   = address of the literal pool entry
 *   isLiteralPool = true
 *
 * ## What we produce
 *
 * For each such reference:
 *   1. Read the 4-byte (ARM32) or 8-byte (AArch64) value at targetVma.
 *   2. If the value is a data address, emit a LiteralPoolEntry with
 *      isPointer=true.
 *   3. If the value looks like a string pointer, recursively classify
 *      the string (done in ref_anchored.cpp via processLiteralPoolEntry).
 *   4. Mark the pool entry address as DATA so the disassembler skips it.
 *
 * This file implements `extractLiteralPool`, which batch-processes a list
 * of InstrRef records.
 */

#include "retdec/string_detect/string_detect.h"
#include <algorithm>
#include <vector>

namespace retdec {
namespace string_detect {

std::vector<LiteralPoolEntry> extractLiteralPool(
    const IBinaryView&           view,
    const std::vector<InstrRef>& instrRefs)
{
    std::vector<LiteralPoolEntry> result;
    result.reserve(instrRefs.size());

    // Track already-processed pool addresses (avoid duplicates)
    std::vector<uint64_t> seen;
    seen.reserve(instrRefs.size());

    for (auto& ref : instrRefs) {
        if (!ref.isLiteralPool) continue;
        if (!view.isMapped(ref.targetVma)) continue;

        // Deduplicate
        auto it = std::lower_bound(seen.begin(), seen.end(), ref.targetVma);
        if (it != seen.end() && *it == ref.targetVma) continue;
        seen.insert(it, ref.targetVma);

        uint32_t ptrW = view.pointerWidth();
        uint8_t  buf[8] = {};
        std::size_t got = view.readBytes(ref.targetVma, buf, ptrW);
        if (got < ptrW) continue;

        uint64_t value = 0;
        for (uint32_t i = 0; i < ptrW; ++i)
            value |= ((uint64_t)buf[i] << (8 * i));

        LiteralPoolEntry entry;
        entry.vma       = ref.targetVma;
        entry.value     = value;
        entry.width     = ptrW;
        entry.isPointer = view.isMapped(value);
        result.push_back(entry);
    }

    return result;
}

} // namespace string_detect
} // namespace retdec
