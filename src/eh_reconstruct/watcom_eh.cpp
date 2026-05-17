/**
 * @file src/eh_reconstruct/watcom_eh.cpp
 * @brief Open Watcom C++ exception handling reconstruction.
 *
 * ## Watcom EH model (32-bit x86)
 *
 * Open Watcom C++ stores exception handling metadata in a dedicated section
 * named `.eh_data` (or `.CW_EXCEP` in older builds).  The section uses a
 * fixed-format binary layout with a magic header for identification.
 *
 * ### .eh_data section layout
 *
 *   struct WExcTableHdr {
 *     char     magic[4];   // "WEXC" (0x57 0x45 0x58 0x43)
 *     uint32_t version;    // currently 1
 *     uint32_t entryCount; // number of WExcEntry records
 *   };                     // total: 12 bytes
 *
 *   struct WExcEntry {
 *     uint32_t fnStartRVA;     // RVA of function start
 *     uint32_t fnEndRVA;       // RVA of function end (exclusive)
 *     uint32_t handlerTableRVA;// RVA of WHandlerEntry[]
 *   };                          // total: 12 bytes
 *
 * Each WHandlerEntry (12 bytes) describes one try/catch block:
 *
 *   struct WHandlerEntry {
 *     uint16_t tryStart;     // byte offset from function start to try begin
 *     uint16_t tryEnd;       // byte offset from function start to try end
 *     uint16_t catchOffset;  // byte offset from function start to catch body
 *     uint16_t flags;        // bit0=reference catch, bit1=cleanup
 *     uint32_t typeNameRVA;  // RVA of plain C string type name (0 = catch-all)
 *   };
 *
 * A zero-filled entry (all fields == 0) terminates a handler table.
 *
 * ### Callee-save register saves
 *
 * Open Watcom's __watcall convention uses EAX, EBX, ECX, EDX as scratch,
 * and ESI, EDI, EBP as callee-save.  We scan the first 32 bytes of each
 * function for PUSH ESI (0x56), PUSH EDI (0x57), PUSH EBX (0x53) and
 * build RegSave records accordingly.
 *
 * ### Fallback: .CW_EXCEP section
 *
 * Older Watcom binaries used the section name ".CW_EXCEP" with the same
 * structure.  We try both.
 *
 * ### Type names
 *
 * Watcom uses plain unmangled C++ class names for exception matching
 * (e.g., "std::exception").  The typeNameRVA points directly to a
 * NUL-terminated ASCII string.
 */

#include <memory>
#include "retdec/eh_reconstruct/eh_reconstruct.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace retdec {
namespace eh_reconstruct {

// ─── Magic constant ───────────────────────────────────────────────────────────

static constexpr uint32_t kWatcomMagic = 0x43584557u; // "WEXC" LE

// ─── Callee-save register scan ────────────────────────────────────────────────

static void parseWatcomRegSaves(const IBinaryView& view,
                                 uint64_t fnStart,
                                 UnwindInfo& info) {
    static const struct { uint8_t op; uint32_t regId; const char* name; } saves[] = {
        { 0x53, 3,  "ebx" },
        { 0x55, 5,  "ebp" },
        { 0x56, 6,  "esi" },
        { 0x57, 7,  "edi" },
    };

    int32_t stackOff = 4;  // return address already pushed
    for (int i = 0; i < 32; ++i) {
        if (!view.isMapped(fnStart + i)) break;
        uint8_t b = view.readU8(fnStart + i);
        for (auto& s : saves) {
            if (b == s.op) {
                RegSave rs;
                rs.regId      = s.regId;
                rs.regName    = s.name;
                rs.frameOffset= -(int32_t)stackOff;
                rs.isXmm      = false;
                info.regSaves.push_back(rs);
                stackOff += 4;
                break;
            }
        }
        if (b == 0xE8 || b == 0xE9 || b == 0xFF) break;  // call / jmp
    }
}

// ─── Parse handler table ──────────────────────────────────────────────────────

static void parseWatcomHandlerTable(const IBinaryView& view,
                                     uint64_t base,
                                     uint64_t tableVma,
                                     uint64_t fnStart,
                                     EHFunction& fn) {
    for (int i = 0; i < 256; ++i) {
        uint64_t entryVma = tableVma + (uint64_t)i * 12;
        if (!view.isMapped(entryVma + 11)) break;

        uint16_t tryStart   = view.readU16LE(entryVma + 0);
        uint16_t tryEnd     = view.readU16LE(entryVma + 2);
        uint16_t catchOff   = view.readU16LE(entryVma + 4);
        uint16_t flags      = view.readU16LE(entryVma + 6);
        uint32_t typeRVA    = view.readU32LE(entryVma + 8);

        // Terminator
        if (tryStart == 0 && tryEnd == 0 && catchOff == 0 && flags == 0 && typeRVA == 0)
            break;
        if (tryEnd <= tryStart) continue;

        TryCatchBlock block;
        block.tryBegin = fnStart + tryStart;
        block.tryEnd   = fnStart + tryEnd;

        CatchHandler ch;
        ch.handlerVma = fnStart + catchOff;
        ch.isCleanup  = (flags & 0x02) != 0;

        if (!ch.isCleanup) {
            if (typeRVA == 0) {
                ch.isCatchAll = true;
                ch.catchType  = "...";
            } else {
                uint64_t nameVma = base + typeRVA;
                if (!view.isMapped(nameVma)) nameVma = typeRVA;
                if (view.isMapped(nameVma)) {
                    char buf[256] = {};
                    view.readBytes(nameVma, reinterpret_cast<uint8_t*>(buf),
                                   sizeof(buf) - 1);
                    ch.catchType = buf;
                } else {
                    ch.catchType = "exception";
                }
            }
        }

        block.handlers.push_back(std::move(ch));
        fn.tryCatchBlocks.push_back(std::move(block));
        fn.hasEH = true;
    }
}

// ─── Watcom EH parser ─────────────────────────────────────────────────────────

class WatcomEHParser final : public IEHParser {
public:
    const char* name() const noexcept override { return "Watcom-EH"; }

    std::vector<EHFunction> parse(const IBinaryView& view) const override {
        std::vector<EHFunction> results;
        uint64_t base = view.imageBase();

        // Try both section names
        static const char* sectionNames[] = { ".eh_data", ".CW_EXCEP", nullptr };

        uint64_t ehVma  = 0;
        std::size_t ehSz = 0;
        for (auto* name = sectionNames; *name; ++name) {
            ehVma = view.sectionVma(*name);
            ehSz  = view.sectionSize(*name);
            if (ehVma && ehSz >= 12) break;
        }
        if (ehVma == 0 || ehSz < 12) return results;

        // Check magic
        uint32_t magic = view.readU32LE(ehVma);
        if (magic != kWatcomMagic) return results;

        // uint32_t version    = view.readU32LE(ehVma + 4);
        uint32_t entryCount = view.readU32LE(ehVma + 8);
        if (entryCount == 0 || entryCount > 65536) return results;

        uint64_t tableStart = ehVma + 12;
        for (uint32_t i = 0; i < entryCount; ++i) {
            uint64_t entryVma = tableStart + (uint64_t)i * 12;
            if (!view.isMapped(entryVma + 11)) break;

            uint32_t fnStartRVA      = view.readU32LE(entryVma + 0);
            uint32_t fnEndRVA        = view.readU32LE(entryVma + 4);
            uint32_t handlerTableRVA = view.readU32LE(entryVma + 8);

            if (fnStartRVA == 0 || fnEndRVA == 0) continue;

            uint64_t fnStart = base + fnStartRVA;
            uint64_t fnEnd   = base + fnEndRVA;
            uint64_t htVma   = (handlerTableRVA != 0) ? (base + handlerTableRVA) : 0;

            EHFunction fn;
            fn.functionVma = fnStart;
            fn.functionEnd = fnEnd;
            fn.unwindInfo.functionBegin = fnStart;
            fn.unwindInfo.functionEnd   = fnEnd;

            parseWatcomRegSaves(view, fnStart, fn.unwindInfo);

            if (htVma != 0 && view.isMapped(htVma)) {
                parseWatcomHandlerTable(view, base, htVma, fnStart, fn);
            }

            results.push_back(std::move(fn));
        }

        return results;
    }
};

std::unique_ptr<IEHParser> makeWatcomEHParser() {
    return std::make_unique<WatcomEHParser>();
}

} // namespace eh_reconstruct
} // namespace retdec
