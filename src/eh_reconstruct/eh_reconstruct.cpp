/**
 * @file src/eh_reconstruct/eh_reconstruct.cpp
 * @brief Core IBinaryView helpers, EHReconstructor, and factory.
 */

#include <memory>
#include "retdec/eh_reconstruct/eh_reconstruct.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace retdec {
namespace eh_reconstruct {

// ─── IBinaryView convenience helpers ─────────────────────────────────────────

uint8_t IBinaryView::readU8(uint64_t vma) const {
    uint8_t b = 0;
    readBytes(vma, &b, 1);
    return b;
}

uint16_t IBinaryView::readU16LE(uint64_t vma) const {
    uint8_t buf[2] = {};
    readBytes(vma, buf, 2);
    return static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
}

uint32_t IBinaryView::readU32LE(uint64_t vma) const {
    uint8_t buf[4] = {};
    readBytes(vma, buf, 4);
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

uint64_t IBinaryView::readU64LE(uint64_t vma) const {
    uint8_t buf[8] = {};
    readBytes(vma, buf, 8);
    uint64_t lo = readU32LE(vma);
    uint64_t hi = readU32LE(vma + 4);
    return lo | (hi << 32);
}

int32_t IBinaryView::readI32LE(uint64_t vma) const {
    return static_cast<int32_t>(readU32LE(vma));
}

uint64_t IBinaryView::readULEB128(uint64_t& cursor) const {
    uint64_t result = 0;
    unsigned shift  = 0;
    while (true) {
        uint8_t b = readU8(cursor++);
        result |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift >= 64) break;
    }
    return result;
}

int64_t IBinaryView::readSLEB128(uint64_t& cursor) const {
    int64_t  result = 0;
    unsigned shift  = 0;
    uint8_t  b      = 0;
    do {
        b = readU8(cursor++);
        result |= (int64_t)(b & 0x7F) << shift;
        shift  += 7;
    } while (b & 0x80);
    // Sign extend
    if (shift < 64 && (b & 0x40))
        result |= -(int64_t)(1ULL << shift);
    return result;
}

uint64_t IBinaryView::readEncodedPtr(uint64_t& cursor,
                                      uint8_t   encoding,
                                      uint64_t  pcrelBase) const {
    if (encoding == 0xFF) return 0;  // DW_EH_PE_omit

    uint8_t application = encoding & 0x70;
    uint8_t format      = encoding & 0x0F;
    bool    indirect    = (encoding & 0x80) != 0;

    uint64_t base = 0;
    if (application == 0x10)       base = pcrelBase;  // pcrel
    else if (application == 0x30)  base = imageBase(); // datarel
    // funcrel (0x40) and textrel (0x20) need external info; ignore for now

    uint64_t value = 0;
    switch (format) {
    case 0x00: // absptr (native word size; assume 8 bytes)
        value = readU64LE(cursor); cursor += 8;
        break;
    case 0x01: // uleb128
        value = readULEB128(cursor);
        break;
    case 0x02: // udata2
        value = readU16LE(cursor); cursor += 2;
        break;
    case 0x03: // udata4
        value = readU32LE(cursor); cursor += 4;
        break;
    case 0x04: // udata8
        value = readU64LE(cursor); cursor += 8;
        break;
    case 0x09: // sleb128
        value = (uint64_t)readSLEB128(cursor);
        break;
    case 0x0a: { // sdata2
        int16_t sv; uint8_t b[2]; readBytes(cursor, b, 2); cursor += 2;
        std::memcpy(&sv, b, 2);
        value = (uint64_t)(int64_t)sv;
        break;
    }
    case 0x0b: { // sdata4
        int32_t sv; uint8_t b[4]; readBytes(cursor, b, 4); cursor += 4;
        std::memcpy(&sv, b, 4);
        value = (uint64_t)(int64_t)sv;
        break;
    }
    case 0x0c: { // sdata8
        int64_t sv; uint8_t b[8]; readBytes(cursor, b, 8); cursor += 8;
        std::memcpy(&sv, b, 8);
        value = (uint64_t)sv;
        break;
    }
    default:
        // Unknown format; skip 4 bytes as a fallback
        cursor += 4;
        break;
    }

    value += base;

    if (indirect && value && isMapped(value)) {
        uint64_t tmp = value;
        value = readU64LE(tmp);
    }

    return value;
}

// ─── Try/catch block nesting ──────────────────────────────────────────────────

/**
 * Given a flat list of TryCatchBlocks (sorted by tryBegin), build a nested
 * tree where inner blocks become `nested` children of outer blocks.
 *
 * Nesting criterion: inner.tryBegin >= outer.tryBegin &&
 *                    inner.tryEnd   <= outer.tryEnd &&
 *                    inner != outer
 */
static std::vector<TryCatchBlock> nestBlocks(std::vector<TryCatchBlock> flat) {
    // Sort by range size descending so outer blocks come first
    std::sort(flat.begin(), flat.end(), [](const TryCatchBlock& a,
                                           const TryCatchBlock& b) {
        uint64_t sza = a.tryEnd - a.tryBegin;
        uint64_t szb = b.tryEnd - b.tryBegin;
        if (sza != szb) return sza > szb;
        return a.tryBegin < b.tryBegin;
    });

    std::vector<TryCatchBlock> roots;

    // Recursive lambda to insert a block into the correct parent
    std::function<bool(std::vector<TryCatchBlock>&, TryCatchBlock)> insertInto;
    insertInto = [&](std::vector<TryCatchBlock>& siblings,
                     TryCatchBlock blk) -> bool {
        for (auto& sib : siblings) {
            if (blk.tryBegin >= sib.tryBegin && blk.tryEnd <= sib.tryEnd) {
                if (!insertInto(sib.nested, blk)) {
                    sib.nested.push_back(std::move(blk));
                }
                return true;
            }
        }
        return false;
    };

    for (auto& blk : flat) {
        if (!insertInto(roots, blk)) {
            roots.push_back(std::move(blk));
        }
    }

    return roots;
}

// ─── EHReconstructor ─────────────────────────────────────────────────────────

EHReconstructor::EHReconstructor() = default;
EHReconstructor::~EHReconstructor() = default;

void EHReconstructor::addParser(std::unique_ptr<IEHParser> parser) {
    parsers_.push_back(std::move(parser));
}

std::size_t EHReconstructor::reconstruct(const IBinaryView& view) {
    functions_.clear();

    for (auto& parser : parsers_) {
        auto parsed = parser->parse(view);
        for (auto& fn : parsed) {
            // Nest flat try blocks
            fn.tryCatchBlocks = nestBlocks(std::move(fn.tryCatchBlocks));

            if (cb_) cb_(fn);
            functions_.push_back(std::move(fn));
        }
    }

    // Sort by function VMA for binary search
    std::sort(functions_.begin(), functions_.end(),
              [](const EHFunction& a, const EHFunction& b) {
                  return a.functionVma < b.functionVma;
              });

    return functions_.size();
}

const EHFunction* EHReconstructor::findFunction(uint64_t vma) const {
    if (functions_.empty()) return nullptr;

    // Binary search for the function whose range contains vma
    auto it = std::upper_bound(functions_.begin(), functions_.end(), vma,
        [](uint64_t v, const EHFunction& fn) {
            return v < fn.functionVma;
        });

    if (it != functions_.begin()) {
        --it;
        if (vma >= it->functionVma && vma < it->functionEnd)
            return &*it;
    }
    return nullptr;
}

const TryCatchBlock* EHReconstructor::findInnermostTry(
    const EHFunction& func, uint64_t vma) {

    std::function<const TryCatchBlock*(const std::vector<TryCatchBlock>&)> find;
    find = [&](const std::vector<TryCatchBlock>& blocks) -> const TryCatchBlock* {
        for (auto& b : blocks) {
            if (vma >= b.tryBegin && vma < b.tryEnd) {
                // Try to find a more specific nested block first
                auto* inner = find(b.nested);
                return inner ? inner : &b;
            }
        }
        return nullptr;
    };

    return find(func.tryCatchBlocks);
}

// ─── Factory ─────────────────────────────────────────────────────────────────

EHReconstructor makeDefaultReconstructor() {
    EHReconstructor rec;
    // Table-driven parsers first (most common toolchains)
    rec.addParser(makeMsvcEHParser());       // PE .pdata RUNTIME_FUNCTION + FuncInfo
    rec.addParser(makeItaniumEHParser());    // ELF .eh_frame + LSDA (x86, x86-64, MIPS, PPC)
    rec.addParser(makeArmEhabiParser());     // ELF .ARM.exidx + .ARM.extab (ARM32)
    // Section-based parsers
    rec.addParser(makeWatcomEHParser());     // Open Watcom .eh_data / .CW_EXCEP
    // Pattern-scanning parsers (run last; may overlap with MSVC/Itanium hits)
    rec.addParser(makeBorlandEHParser());    // Borland/Embarcadero FS:[0] frame chain
    rec.addParser(makeDmcEHParser());        // Digital Mars C++ FS:[0] scope table
    rec.addParser(makeSymbianEHParser());    // Symbian/EPOC TTrap::Trap pattern
    return rec;
}

} // namespace eh_reconstruct
} // namespace retdec
