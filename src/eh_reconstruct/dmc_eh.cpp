/**
 * @file src/eh_reconstruct/dmc_eh.cpp
 * @brief Digital Mars C++ (DMC) exception handling reconstruction.
 *
 * ## DMC EH model (32-bit x86)
 *
 * Digital Mars C++ uses a proprietary frame-based exception handling model
 * that is structurally similar to Microsoft's old x86 SEH (VC6 __except)
 * but uses different structure layouts and handler functions.
 *
 * ### Function prolog
 *
 * Every DMC function with try/catch installs an exception frame at the top
 * of the stack using the same FS:[0] mechanism as MSVC x86:
 *
 *   PUSH imm32          ; handler function (often __DMCexcept or _except_handler3)
 *   PUSH DWORD PTR FS:[0]
 *   MOV  DWORD PTR FS:[0], ESP
 *   SUB  ESP, <local_size>
 *   PUSH imm32          ; pointer to scope table
 *   MOV  DWORD PTR [ESP+local_size+12], -1  ; initial try-level = TRYLEVEL_NONE
 *
 * The exception frame (DMCExcFrame) on the stack:
 *
 *   struct DMCExcFrame {
 *     DMCExcFrame* prev;      // previous exception frame (was FS:[0])
 *     void*        handler;   // __DMCexcept or __except_handler3
 *     uint32_t     tryLevel;  // current try nesting depth (-1 = none)
 *     DMCScopeEntry* pScope;  // pointer to scope table
 *     uint32_t*    ebpSaved;  // saved EBP
 *   };
 *
 * ### Scope table (DMCScopeEntry)
 *
 * The scope table is an array of entries, one per try/catch block,
 * terminated by an entry with enclosingLevel == TRYLEVEL_NONE (-1):
 *
 *   struct DMCScopeEntry {
 *     int32_t  enclosingLevel; // nesting level of the enclosing try (-1 = top-level)
 *     void*    filterFn;       // NULL = __finally; non-NULL = catch type check
 *     void*    handlerFn;      // catch body or __finally body
 *   };
 *
 * If filterFn == NULL → cleanup (__finally) block.
 * If filterFn != NULL → the filter is a C++ type-filter thunk. The thunk's
 *   first instruction is often: PUSH <TypeDescriptor*>; JMP __TypeMatch
 *   We extract the TypeDescriptor pointer from that thunk.
 *
 * ### TypeDescriptor (same as MSVC 32-bit)
 *
 *   struct TypeDescriptor {
 *     void* pVFTable;   // __type_info vftable (4 bytes in 32-bit)
 *     void* spare;
 *     char  name[];     // mangled name, e.g. "?AV<classname>@@"
 *   };
 *
 * ### Detection strategy
 *
 * 1. Scan .text for the same FS:[0] prolog pattern as Borland.
 * 2. The key difference: the handler address from the prolog is either
 *    __DMCexcept or __except_handler3 (we recognize both by name if
 *    available in the symbol table, or by matching the first opcode).
 * 3. The PUSH imm32 immediately after MOV FS:[0],ESP is the scope table ptr.
 *    We look for that PUSH within the next 20 bytes after the prolog.
 * 4. Parse DMCScopeEntry[] from the scope table pointer.
 * 5. For each entry with filterFn != NULL, follow to the type thunk and
 *    extract the TypeDescriptor pointer.
 */

#include <memory>
#include "retdec/eh_reconstruct/eh_reconstruct.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace retdec {
namespace eh_reconstruct {

// ─── DMC prolog pattern ───────────────────────────────────────────────────────

// Same FS:[0] prolog as Borland, reuse the same byte pattern detection.
// Bytes 0..18:
//   0x68 <4 bytes handler>  0x64 0xFF 0x35 0x00 0x00 0x00 0x00
//   0x64 0x89 0x25 0x00 0x00 0x00 0x00
static bool matchDmcProlog(const IBinaryView& view, uint64_t vma,
                            uint32_t& handlerAddr) {
    if (view.readU8(vma) != 0x68) return false;
    if (view.readU8(vma + 5) != 0x64) return false;
    if (view.readU8(vma + 6) != 0xFF || view.readU8(vma + 7) != 0x35) return false;
    if (view.readU32LE(vma + 8) != 0) return false;
    if (view.readU8(vma + 12) != 0x64) return false;
    if (view.readU8(vma + 13) != 0x89 || view.readU8(vma + 14) != 0x25) return false;
    if (view.readU32LE(vma + 15) != 0) return false;
    handlerAddr = view.readU32LE(vma + 1);
    return true;
}

// ─── DMC type filter thunk analysis ──────────────────────────────────────────

/**
 * Extract the TypeDescriptor pointer from a DMC type-filter thunk.
 * The thunk typically looks like:
 *   PUSH imm32   ; TypeDescriptor*
 *   JMP/CALL __TypeMatch
 * Returns 0 if not recognized.
 */
static uint32_t extractTypeFromFilterThunk(const IBinaryView& view,
                                            uint64_t thunkVma) {
    if (!view.isMapped(thunkVma)) return 0;

    // Look for PUSH imm32 (0x68) within first 4 bytes
    for (int i = 0; i < 4; ++i) {
        if (view.readU8(thunkVma + i) == 0x68) {
            return view.readU32LE(thunkVma + i + 1);
        }
    }
    return 0;
}

// ─── Parse DMC TypeDescriptor ─────────────────────────────────────────────────

static std::string readDmcTypeName(const IBinaryView& view, uint64_t typeDescVma) {
    if (typeDescVma == 0 || !view.isMapped(typeDescVma)) return "...";
    // Layout (32-bit): vftable(4) + spare(4) + name[]
    uint64_t nameVma = typeDescVma + 8;
    if (!view.isMapped(nameVma)) return "...";

    char buf[256] = {};
    view.readBytes(nameVma, reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);

    // Strip leading "?AV" or "?AU" decoration if present
    const char* p = buf;
    if (p[0] == '?' && p[1] == 'A' && (p[2] == 'V' || p[2] == 'U' || p[2] == 'T'))
        p += 3;
    // Strip trailing "@"
    std::string name(p);
    auto at = name.find('@');
    if (at != std::string::npos) name = name.substr(0, at);
    return name.empty() ? "exception" : name;
}

// ─── Parse DMCScopeEntry table ────────────────────────────────────────────────

static void parseDmcScopeTable(const IBinaryView& view,
                                 uint64_t base,
                                 uint64_t tableVma,
                                 uint64_t fnStart,
                                 EHFunction& fn) {
    // Scan up to 128 entries; stop at entry with all-zero or enclosingLevel sentinel
    // Group entries by enclosingLevel into try blocks.

    struct Entry {
        int32_t  enclosingLevel;
        uint32_t filterFn;
        uint32_t handlerFn;
    };

    std::vector<Entry> entries;
    for (int i = 0; i < 128; ++i) {
        uint64_t entryVma = tableVma + (uint64_t)i * 12;
        if (!view.isMapped(entryVma + 11)) break;

        Entry e;
        e.enclosingLevel = view.readI32LE(entryVma + 0);
        e.filterFn       = view.readU32LE(entryVma + 4);
        e.handlerFn      = view.readU32LE(entryVma + 8);

        // Terminator: TRYLEVEL_NONE sentinel
        if (e.enclosingLevel == -1 && e.filterFn == 0 && e.handlerFn == 0) break;

        entries.push_back(e);
    }

    if (entries.empty()) return;

    // Build one TryCatchBlock per unique sequence of handlers.
    // For simplicity: group consecutive entries into one block;
    // nested try blocks are identified by differing enclosingLevel.
    TryCatchBlock block;
    block.tryBegin = fnStart;
    block.tryEnd   = fnStart + 0x1000;  // tighten via CFG later

    for (auto& e : entries) {
        CatchHandler ch;
        uint64_t handlerVma = (uint64_t)e.handlerFn;
        if (!view.isMapped(handlerVma)) handlerVma = base + e.handlerFn;
        ch.handlerVma = handlerVma;

        if (e.filterFn == 0) {
            // __finally / cleanup
            ch.isCleanup = true;
            ch.catchType = "";
        } else {
            // Type-filter thunk
            uint64_t filterVma = (uint64_t)e.filterFn;
            if (!view.isMapped(filterVma)) filterVma = base + e.filterFn;

            uint32_t typePtr = extractTypeFromFilterThunk(view, filterVma);
            uint64_t typeVma = (uint64_t)typePtr;
            if (!view.isMapped(typeVma)) typeVma = base + typePtr;

            if (typePtr == 0) {
                ch.isCatchAll = true;
                ch.catchType  = "...";
            } else {
                ch.catchType = readDmcTypeName(view, typeVma);
            }
        }

        block.handlers.push_back(std::move(ch));
    }

    if (!block.handlers.empty()) {
        fn.tryCatchBlocks.push_back(std::move(block));
        fn.hasEH = true;
    }
}

// ─── DMC EH parser ────────────────────────────────────────────────────────────

class DmcEHParser final : public IEHParser {
public:
    const char* name() const noexcept override { return "DMC-EH"; }

    std::vector<EHFunction> parse(const IBinaryView& view) const override {
        std::vector<EHFunction> results;

        uint64_t textVma  = view.sectionVma(".text");
        std::size_t textSz = view.sectionSize(".text");
        if (textVma == 0 || textSz < 20) return results;

        uint64_t base = view.imageBase();

        for (std::size_t off = 0; off + 19 <= textSz; ++off) {
            uint64_t vma = textVma + off;
            uint32_t handlerAddr = 0;
            if (!matchDmcProlog(view, vma, handlerAddr)) continue;

            uint64_t fnStart = vma;
            for (int back = 1; back <= 4; ++back) {
                if (view.readU8(vma - back) == 0x55) { fnStart = vma - back; break; }
            }

            // Find the scope table pointer: look for PUSH imm32 within 20 bytes
            // after the prolog end (vma + 19)
            uint64_t tableVma = 0;
            for (int i = 0; i < 32; ++i) {
                if (view.readU8(vma + 19 + i) == 0x68) {
                    uint32_t tblRaw = view.readU32LE(vma + 19 + i + 1);
                    uint64_t tblVma = (uint64_t)tblRaw;
                    if (!view.isMapped(tblVma)) tblVma = base + tblRaw;
                    if (view.isMapped(tblVma) && view.isDataSection(tblVma)) {
                        tableVma = tblVma;
                    }
                    break;
                }
                // Also look for MOV [ESP+N], imm32 patterns
                if (view.readU8(vma + 19 + i) == 0xC7) break;
            }

            EHFunction fn;
            fn.functionVma = fnStart;
            fn.functionEnd = fnStart + 0x1000;
            fn.unwindInfo.functionBegin = fnStart;
            fn.unwindInfo.functionEnd   = fn.functionEnd;

            if (tableVma != 0) {
                parseDmcScopeTable(view, base, tableVma, fnStart, fn);
            }

            results.push_back(std::move(fn));
            off += 18;  // skip rest of prolog
        }

        return results;
    }
};

std::unique_ptr<IEHParser> makeDmcEHParser() {
    return std::make_unique<DmcEHParser>();
}

} // namespace eh_reconstruct
} // namespace retdec
