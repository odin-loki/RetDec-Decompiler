/**
 * @file src/string_detect/ref_anchored.cpp
 * @brief StringDetector core: processRef and the known-address set.
 *
 * ## Algorithm
 *
 * For each InstrRef from the instruction decoder:
 *
 *   1. If already in the known-string set → skip (deduplicate).
 *   2. If it is a literal pool entry (isLiteralPool==true):
 *      - Read the 4 or 8-byte literal value.
 *      - If the value is a valid data address, recurse to processRef.
 *      - Mark the address as DATA so the disassembler does not decode it.
 *   3. Otherwise:
 *      a. Try typeString() at targetVma.
 *      b. If it returns a valid StringLiteral, emit it.
 *      c. If the target is inside a data section and points to a region of
 *         consecutive pointers, try detectStringTable().
 *      d. If no string found and the target is in a data section, try reading
 *         at pointer-sized offset (common for std::string internal structure
 *         where the pointer field is at offset 0).
 *
 * ## Deduplication
 *
 * We keep knownStringAddrs_ sorted.  Binary search on each processRef call.
 * Memory is O(#strings), lookup is O(log n).
 *
 * ## Zero-false-positive guarantee
 *
 * We ONLY emit a StringLiteral if:
 *   - The address was reached via an instruction reference (not blind scan).
 *   - typeString() validated the byte content (printable bytes + proper term).
 *   - Minimum charCount (cfg_.minStringLen) met.
 */

#include "retdec/string_detect/string_detect.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace retdec {
namespace string_detect {

// ─── StringDetector ───────────────────────────────────────────────────────────

StringDetector::StringDetector(const IBinaryView&         view,
                               const StringDetectorConfig& cfg)
    : view_(view), cfg_(cfg) {}

StringDetector::~StringDetector() = default;

bool StringDetector::isKnownString(uint64_t vma) const {
    return std::binary_search(knownStringAddrs_.begin(),
                               knownStringAddrs_.end(), vma);
}

bool StringDetector::isDataMarked(uint64_t vma) const {
    return std::binary_search(dataMarkedAddrs_.begin(),
                               dataMarkedAddrs_.end(), vma);
}

void StringDetector::emitString(StringLiteral s) {
    // Insert address into known set (keep sorted)
    auto pos = std::lower_bound(knownStringAddrs_.begin(),
                                 knownStringAddrs_.end(), s.address);
    if (pos == knownStringAddrs_.end() || *pos != s.address) {
        knownStringAddrs_.insert(pos, s.address);
        strings_.push_back(s);
        if (strCb_) strCb_(strings_.back());
    }
}

void StringDetector::processLiteralPoolEntry(const InstrRef& ref) {
    if (!cfg_.detectLitPool) return;

    // Read the literal value
    uint32_t ptrW  = view_.pointerWidth();
    uint8_t  buf[8] = {};
    if (view_.readBytes(ref.targetVma, buf, ptrW) < ptrW) return;

    uint64_t value = 0;
    for (uint32_t i=0; i<ptrW; ++i)
        value |= ((uint64_t)buf[i] << (8*i));

    LiteralPoolEntry entry;
    entry.vma       = ref.targetVma;
    entry.value     = value;
    entry.width     = ptrW;
    entry.isPointer = view_.isMapped(value);

    // Mark the pool entry address as DATA
    auto pos = std::lower_bound(dataMarkedAddrs_.begin(),
                                 dataMarkedAddrs_.end(), ref.targetVma);
    if (pos == dataMarkedAddrs_.end() || *pos != ref.targetVma) {
        dataMarkedAddrs_.insert(pos, ref.targetVma);
    }

    litPool_.push_back(entry);
    if (litCb_) litCb_(litPool_.back());

    // If the literal value is a data address, attempt string classification
    if (entry.isPointer && view_.isDataSection(value)) {
        InstrRef synthetic;
        synthetic.instrVma    = ref.instrVma;
        synthetic.targetVma   = value;
        synthetic.isLiteralPool = false;
        processRef(synthetic);
    }
}

void StringDetector::processRef(const InstrRef& ref) {
    if (ref.targetVma == 0) return;

    // ARM literal pool entry
    if (ref.isLiteralPool) {
        processLiteralPoolEntry(ref);
        return;
    }

    // Deduplication check
    if (isKnownString(ref.targetVma)) return;

    // Only attempt string classification if the target is in a data section,
    // OR is in a code section (ARM literal pool data embedded in .text).
    if (!view_.isMapped(ref.targetVma)) return;

    // Attempt string classification
    auto sl = typeString(view_, ref.targetVma, cfg_.maxStringLen);
    if (sl && sl->charCount >= cfg_.minStringLen) {
        sl->refVma          = ref.instrVma;
        sl->inCodeSection   = view_.isCodeSection(ref.targetVma);
        emitString(std::move(*sl));
        return;
    }

    // If target is in data section, check if it looks like a string table
    if (cfg_.detectTables && view_.isDataSection(ref.targetVma)) {
        auto table = detectStringTable(view_, ref.targetVma,
                                        cfg_.tableMinEntries > 0
                                            ? 1024 : cfg_.tableMinEntries);
        if (table && table->count >= cfg_.tableMinEntries) {
            // Emit each entry as a separate string
            for (uint64_t target : table->targets) {
                if (isKnownString(target)) continue;
                auto entry = typeString(view_, target, cfg_.maxStringLen);
                if (entry && entry->charCount >= cfg_.minStringLen) {
                    entry->refVma       = ref.instrVma;
                    entry->isTableEntry = true;
                    emitString(std::move(*entry));
                }
            }
            tables_.push_back(std::move(*table));
            if (tableCb_) tableCb_(tables_.back());
        }
    }
}

std::size_t StringDetector::processRefs(const std::vector<InstrRef>& refs) {
    std::size_t before = strings_.size();
    for (auto& r : refs) processRef(r);
    return strings_.size() - before;
}

void StringDetector::processCompareBranch(int64_t  compareImm,
                                           uint64_t branchVma,
                                           uint64_t inlinePath,
                                           uint64_t heapPath)
{
    if (!cfg_.detectSSO) return;
    auto info = detectSSOBranch(compareImm, branchVma, inlinePath, heapPath);
    if (info) {
        sso_.push_back(*info);
        if (ssoCb_) ssoCb_(sso_.back());
    }
}

void StringDetector::scanRegion(uint64_t start, uint64_t end) {
    if (!cfg_.detectTables) return;
    if (start >= end) return;

    // Walk the region in pointer-width strides looking for string tables
    uint32_t ptrW = view_.pointerWidth();
    uint64_t vma  = start;
    while (vma + ptrW <= end) {
        if (!isKnownString(vma) && !isDataMarked(vma)) {
            auto table = detectStringTable(view_, vma);
            if (table && table->count >= cfg_.tableMinEntries) {
                for (uint64_t target : table->targets) {
                    if (!isKnownString(target)) {
                        auto sl = typeString(view_, target, cfg_.maxStringLen);
                        if (sl && sl->charCount >= cfg_.minStringLen) {
                            sl->isTableEntry = true;
                            emitString(std::move(*sl));
                        }
                    }
                }
                tables_.push_back(std::move(*table));
                if (tableCb_) tableCb_(tables_.back());
                vma += tables_.back().count * ptrW;
                continue;
            }
        }
        vma += ptrW;
    }
}

} // namespace string_detect
} // namespace retdec
