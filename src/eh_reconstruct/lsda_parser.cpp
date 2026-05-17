/**
 * @file src/eh_reconstruct/lsda_parser.cpp
 * @brief Shared Itanium-format LSDA parser used by both the .eh_frame (Itanium)
 *        and ARM EHABI EH parsers.
 */

#include "lsda_parser.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace retdec {
namespace eh_reconstruct {

namespace {

/// Read the human-readable name of a std::type_info object at `typeInfoVma`.
/// Itanium ABI layout: { vtable_ptr, char* __name }.  We read the __name
/// pointer (at offset 8 on 64-bit) and strip the leading mangling prefix.
static std::string readTypeInfoName(const IBinaryView& view,
                                     uint64_t typeInfoVma) {
    if (typeInfoVma == 0 || !view.isMapped(typeInfoVma)) return "...";

    uint64_t namePtr = view.readU64LE(typeInfoVma + 8);
    if (namePtr == 0 || !view.isMapped(namePtr)) return "type_info";

    char nameBuf[256] = {};
    view.readBytes(namePtr, reinterpret_cast<uint8_t*>(nameBuf), sizeof(nameBuf) - 1);

    const char* n = nameBuf;
    // Strip '_ZTS' (type-string) or '_ZTI' (type-info) prefix
    if (n[0] == '_' && n[1] == 'Z' && n[2] == 'T' &&
        (n[3] == 'S' || n[3] == 'I'))
        n += 4;
    // Skip length prefix used in nested names
    while (*n >= '0' && *n <= '9') ++n;
    std::string result(n);
    if (!result.empty() && result.back() == 'E') result.pop_back();
    return result.empty() ? nameBuf : result;
}

} // anonymous namespace

// ─── parseLSDA ────────────────────────────────────────────────────────────────

LsdaResult parseLSDA(const IBinaryView& view,
                     uint64_t lsdaVma,
                     uint64_t funcStart) {
    LsdaResult res;
    uint64_t cur = lsdaVma;

    // lpstart encoding + optional value
    uint8_t  lpstartEnc = view.readU8(cur++);
    uint64_t lpstart    = funcStart;
    if (lpstartEnc != DW_EH_PE_omit)
        lpstart = view.readEncodedPtr(cur, lpstartEnc, cur);

    // ttype encoding + optional base offset
    uint8_t  ttypeEnc  = view.readU8(cur++);
    uint64_t ttypeBase = 0;
    if (ttypeEnc != DW_EH_PE_omit) {
        uint64_t ttypeOff = view.readULEB128(cur);
        ttypeBase = cur + ttypeOff;
    }

    // Call-site table
    uint8_t  csEnc      = view.readU8(cur++);
    uint64_t csTableLen = view.readULEB128(cur);
    uint64_t csTableEnd = cur + csTableLen;

    while (cur < csTableEnd) {
        uint64_t csStart = view.readEncodedPtr(cur, csEnc, lsdaVma);
        uint64_t csLen   = view.readEncodedPtr(cur, csEnc, lsdaVma);
        uint64_t lp      = view.readEncodedPtr(cur, csEnc, lsdaVma);
        uint64_t action  = view.readULEB128(cur);

        LsdaResult::Site site;
        site.csStart    = lpstart + csStart;
        site.csEnd      = lpstart + csStart + csLen;
        site.landingPad = (lp != 0) ? (lpstart + lp) : 0;
        site.action     = static_cast<uint32_t>(action);
        res.sites.push_back(site);
    }

    // Action table: walk each chain using correct Itanium ABI §7.3.5 arithmetic.
    // ar_next is an sleb128 offset from the byte *immediately after* ar_next.
    uint64_t actionTableBase = csTableEnd;
    std::map<uint64_t, std::size_t> actionOffToIdx;

    for (const auto& s : res.sites) {
        if (s.action == 0) continue;
        uint64_t cur2 = actionTableBase + (s.action - 1);
        while (true) {
            if (actionOffToIdx.count(cur2)) break;
            uint64_t before = cur2;
            int64_t  tf     = view.readSLEB128(cur2);
            int64_t  nx     = view.readSLEB128(cur2);

            actionOffToIdx[before] = res.actions.size();
            LsdaResult::Action a;
            a.typeFilter = tf;
            a.nextOffset = nx;
            res.actions.push_back(a);

            if (nx == 0) break;
            // cur2 already points just past the ar_next field;
            // the next entry is at cur2 + nx.
            cur2 = cur2 + nx;
        }
    }

    // Type table (grows downward from ttypeBase; entry i at ttypeBase - i*ptrSize)
    if (ttypeBase != 0 && ttypeEnc != DW_EH_PE_omit) {
        uint8_t     lower   = ttypeEnc & 0x0F;
        std::size_t ptrSize = 8;
        if (lower == DW_EH_PE_udata4 || lower == DW_EH_PE_sdata4) ptrSize = 4;
        else if (lower == DW_EH_PE_udata2 || lower == DW_EH_PE_sdata2) ptrSize = 2;

        int64_t maxIdx = 0;
        for (const auto& a : res.actions)
            if (a.typeFilter > maxIdx) maxIdx = a.typeFilter;

        res.typeTable.resize(static_cast<std::size_t>(maxIdx) + 1, 0);
        for (int64_t i = 1; i <= maxIdx; ++i) {
            uint64_t entryVma = ttypeBase - static_cast<uint64_t>(i) * ptrSize;
            uint64_t tmp      = entryVma;
            uint8_t  enc      = ttypeEnc & ~DW_EH_PE_indirect;
            uint64_t ptr      = view.readEncodedPtr(tmp, enc, lsdaVma);
            if ((ttypeEnc & DW_EH_PE_indirect) && ptr && view.isMapped(ptr)) {
                uint64_t tmp2 = ptr;
                ptr = view.readEncodedPtr(tmp2, DW_EH_PE_absptr, 0);
            }
            res.typeTable[static_cast<std::size_t>(i)] = ptr;
        }
    }

    return res;
}

// ─── lsdaToTryCatch ───────────────────────────────────────────────────────────

void lsdaToTryCatch(const IBinaryView& view,
                    const LsdaResult& lsda,
                    EHFunction& fn) {
    std::map<uint64_t, TryCatchBlock> lpToBlock;

    for (const auto& site : lsda.sites) {
        if (site.landingPad == 0) continue;

        TryCatchBlock& block = lpToBlock[site.landingPad];
        if (block.tryBegin == 0 || site.csStart < block.tryBegin)
            block.tryBegin = site.csStart;
        if (site.csEnd > block.tryEnd)
            block.tryEnd = site.csEnd;
        block.handlers.clear();

        if (site.action == 0) {
            CatchHandler ch;
            ch.handlerVma = site.landingPad;
            ch.isCleanup  = true;
            ch.catchType  = "";
            block.handlers.push_back(ch);
        } else {
            std::size_t actionIdx = site.action - 1;
            while (actionIdx < lsda.actions.size()) {
                const LsdaResult::Action& act = lsda.actions[actionIdx];
                CatchHandler ch;
                ch.handlerVma = site.landingPad;

                if (act.typeFilter == 0) {
                    ch.isCatchAll = true;
                    ch.catchType  = "...";
                } else if (act.typeFilter > 0) {
                    std::size_t idx = static_cast<std::size_t>(act.typeFilter);
                    if (idx < lsda.typeTable.size()) {
                        ch.catchType = readTypeInfoName(view, lsda.typeTable[idx]);
                    } else {
                        ch.catchType = "std::exception";
                    }
                } else {
                    // Negative filter = exception specification, rarely seen — skip.
                    break;
                }

                bool dup = false;
                for (const auto& existing : block.handlers) {
                    if (existing.handlerVma == ch.handlerVma &&
                        existing.catchType  == ch.catchType) {
                        dup = true; break;
                    }
                }
                if (!dup) block.handlers.push_back(std::move(ch));
                if (act.nextOffset == 0) break;
                ++actionIdx;
            }
        }
    }

    for (auto& [lp, block] : lpToBlock) {
        if (!block.handlers.empty()) {
            fn.tryCatchBlocks.push_back(std::move(block));
            fn.hasEH = true;
        }
    }
}

} // namespace eh_reconstruct
} // namespace retdec
