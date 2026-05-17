/**
 * @file src/eh_reconstruct/msvc_eh.cpp
 * @brief MSVC x64 SEH / C++ EH table parser.
 *
 * Parsing overview
 * ────────────────
 *   PE exception directory (IMAGE_DIRECTORY_ENTRY_EXCEPTION = 3) points to
 *   an array of RUNTIME_FUNCTION structs:
 *
 *     struct RUNTIME_FUNCTION {
 *       DWORD BeginAddress;        // RVA of function start
 *       DWORD EndAddress;          // RVA of function end (exclusive)
 *       DWORD UnwindInfoAddress;   // RVA of UNWIND_INFO
 *     };
 *
 *   UNWIND_INFO (at UnwindInfoAddress, all RVAs relative to image base):
 *     BYTE  VersionFlags     (Version=low3, Flags=high5)
 *     BYTE  SizeOfProlog
 *     BYTE  CountOfCodes
 *     BYTE  FrameRegAndOffset (FrameReg=low4, FrameOffset=high4 *16)
 *     UNWIND_CODE UnwindCode[CountOfCodes]  (2 bytes each, some 4 or 6 bytes)
 *     [padding to DWORD if CountOfCodes is odd]
 *     [ExceptionHandler RVA + HandlerData  if UNW_FLAG_EHANDLER set]
 *
 *   UNWIND_CODE:
 *     BYTE CodeOffset   // offset from function start where prolog op completes
 *     BYTE UnwindOp:4   // operation code
 *     BYTE OpInfo:4     // supplemental info (usually the register index)
 *
 *   UNW_FLAG_EHANDLER = 1  → exception handler present  → C++ EH FuncInfo
 *   UNW_FLAG_UHANDLER = 2  → unwind handler present     → C++ EH FuncInfo
 *   UNW_FLAG_CHAININFO= 4  → chained unwind info        → second RUNTIME_FUNCTION
 *
 *   FuncInfo (immediately after handler RVA, DWORD-aligned):
 *     DWORD magicNumber (0x19930520, 0x19930521, 0x19930522)
 *     DWORD maxState
 *     DWORD pUnwindMap        (RVA of UnwindMapEntry[])
 *     DWORD nTryBlocks
 *     DWORD pTryBlockMap      (RVA of TryBlockMapEntry[])
 *     DWORD nIPMapEntries
 *     DWORD pIPtoStateMap     (RVA of IptoStateMapEntry[])
 *     DWORD pESTypeList       (magic>=0x19930521)
 *     DWORD EHFlags           (magic>=0x19930522)
 *
 *   TryBlockMapEntry:
 *     DWORD tryLow, tryHigh   // EH state range of try block
 *     DWORD catchHigh         // EH state after last catch
 *     DWORD nCatches
 *     DWORD pHandlerArray     (RVA of HandlerType[])
 *
 *   HandlerType:
 *     DWORD adjectives        // bit0=const, bit1=volatile, bit2=reference, bit3=obj (not ref)
 *     DWORD pType             // RVA of TypeDescriptor* (0 = catch-all)
 *     DWORD dispCatchObj      // displacement of catch variable on stack frame
 *     DWORD addressOfHandler  // RVA of handler function (or thunk)
 *
 *   TypeDescriptor (at *pType):
 *     QWORD pVFTable          // __type_info vftable
 *     QWORD spare
 *     char  name[]            // mangled decorated name ("?AV..." or ".?AV...")
 */

#include <memory>
#include "retdec/eh_reconstruct/eh_reconstruct.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace retdec {
namespace eh_reconstruct {

// ─── MSVC register name table (x64) ──────────────────────────────────────────

static const char* msvcRegName(unsigned regIdx) {
    // UWOP register encoding for x64
    static const char* names[] = {
        "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8", "r9", "r10","r11","r12","r13","r14","r15"
    };
    if (regIdx < 16) return names[regIdx];
    return "unk";
}

// ─── Unwind operation codes ───────────────────────────────────────────────────

enum UnwindOp : uint8_t {
    UWOP_PUSH_NONVOL     = 0,
    UWOP_ALLOC_LARGE     = 1,
    UWOP_ALLOC_SMALL     = 2,
    UWOP_SET_FPREG       = 3,
    UWOP_SAVE_NONVOL     = 4,
    UWOP_SAVE_NONVOL_FAR = 5,
    UWOP_SAVE_XMM128     = 8,
    UWOP_SAVE_XMM128_FAR = 9,
    UWOP_PUSH_MACH_FRAME = 10,
};

// ─── Demangle MSVC TypeDescriptor name ───────────────────────────────────────

/**
 * MSVC TypeDescriptor names look like ".?AVstd::exception@@" or "?AV..." .
 * Strip the leading ".?" or "?" and trailing "@" decorations.
 * For a proper demangler we'd call __unDName, but for type name recovery
 * we do a minimal structural parse:
 *   .?AV<name>@@    → class <name>
 *   .?AU<name>@@    → struct <name>
 *   .?AV?$<tmpl>... → template class
 */
static std::string demangleMsvcType(const char* raw) {
    if (!raw || raw[0] == '\0') return "...";   // catch-all

    const char* p = raw;
    // Skip leading ".?" or "?"
    if (p[0] == '.') ++p;
    if (p[0] == '?') ++p;

    // Expect A or B (near/far pointer) then V (class) or U (struct) or T (union)
    if (p[0] == 'A' || p[0] == 'B') ++p;

    char typeChar = '\0';
    if (p[0] == 'V' || p[0] == 'U' || p[0] == 'T' || p[0] == 'W') {
        typeChar = p[0];
        ++p;
    }

    // Everything up to first '@' or end is the qualified name
    std::string name;
    while (*p && *p != '@') {
        name += *p++;
    }
    // Replace remaining "@@" namespace separators with "::"
    // Actually strip all trailing '@'
    return name.empty() ? raw : name;
}

// ─── Parse UNWIND_INFO ────────────────────────────────────────────────────────

struct ParsedUnwindInfo {
    UnwindInfo  info;
    bool        hasEHandler = false;  // UNW_FLAG_EHANDLER
    bool        hasUHandler = false;  // UNW_FLAG_UHANDLER
    bool        hasChain    = false;  // UNW_FLAG_CHAININFO
    uint64_t    handlerDataVma = 0;   // VMA after exception handler RVA
    uint64_t    chainRtFnVma   = 0;   // VMA of chained RUNTIME_FUNCTION
};

static ParsedUnwindInfo parseUnwindInfo(const IBinaryView& view,
                                        uint64_t base,
                                        uint64_t fnBegin,
                                        uint64_t fnEnd,
                                        uint64_t unwindVma) {
    ParsedUnwindInfo result;
    result.info.functionBegin = fnBegin;
    result.info.functionEnd   = fnEnd;

    uint64_t cur = unwindVma;

    uint8_t versionFlags = view.readU8(cur++);
    uint8_t version      = versionFlags & 0x07;
    uint8_t flags        = (versionFlags >> 3) & 0x1F;
    (void)version;

    result.hasEHandler = (flags & 1) != 0;
    result.hasUHandler = (flags & 2) != 0;
    result.hasChain    = (flags & 4) != 0;

    uint8_t prologSize    = view.readU8(cur++);
    uint8_t countOfCodes  = view.readU8(cur++);
    uint8_t frameRegOff   = view.readU8(cur++);

    result.info.prologSize    = prologSize;
    result.info.frameReg      = frameRegOff & 0x0F;
    result.info.frameRegOffset= ((frameRegOff >> 4) & 0x0F) * 16;

    // Parse unwind codes
    uint64_t codeCur = cur;
    uint32_t stackOffset = 8;  // return address already pushed

    for (int i = 0; i < countOfCodes; ) {
        uint8_t codeOffset = view.readU8(codeCur);
        uint8_t opByte     = view.readU8(codeCur + 1);
        UnwindOp op  = static_cast<UnwindOp>(opByte & 0x0F);
        uint8_t  reg = (opByte >> 4) & 0x0F;

        switch (op) {
        case UWOP_PUSH_NONVOL: {
            RegSave rs;
            rs.regId      = reg;
            rs.regName    = msvcRegName(reg);
            rs.frameOffset= -(int32_t)stackOffset - 8;
            rs.isXmm      = false;
            result.info.regSaves.push_back(rs);
            stackOffset += 8;
            codeCur += 2; i += 1;
            break;
        }
        case UWOP_ALLOC_LARGE: {
            if (reg == 0) {
                uint16_t slots = view.readU16LE(codeCur + 2);
                result.info.frameSize += slots * 8;
                stackOffset += slots * 8;
                codeCur += 4; i += 2;
            } else {
                uint32_t bytes = view.readU32LE(codeCur + 2);
                result.info.frameSize += bytes;
                stackOffset += bytes;
                codeCur += 6; i += 3;
            }
            break;
        }
        case UWOP_ALLOC_SMALL: {
            uint32_t bytes = (reg + 1) * 8u;
            result.info.frameSize += bytes;
            stackOffset += bytes;
            codeCur += 2; i += 1;
            break;
        }
        case UWOP_SET_FPREG:
            codeCur += 2; i += 1;
            break;
        case UWOP_SAVE_NONVOL: {
            uint16_t offsetSlots = view.readU16LE(codeCur + 2);
            RegSave rs;
            rs.regId      = reg;
            rs.regName    = msvcRegName(reg);
            rs.frameOffset= (int32_t)(offsetSlots * 8u);
            rs.isXmm      = false;
            result.info.regSaves.push_back(rs);
            codeCur += 4; i += 2;
            break;
        }
        case UWOP_SAVE_NONVOL_FAR: {
            uint32_t offsetBytes = view.readU32LE(codeCur + 2);
            RegSave rs;
            rs.regId      = reg;
            rs.regName    = msvcRegName(reg);
            rs.frameOffset= (int32_t)offsetBytes;
            rs.isXmm      = false;
            result.info.regSaves.push_back(rs);
            codeCur += 6; i += 3;
            break;
        }
        case UWOP_SAVE_XMM128: {
            uint16_t offsetSlots = view.readU16LE(codeCur + 2);
            RegSave rs;
            rs.regId      = 16 + reg;   // XMM0 = 16
            rs.regName    = std::string("xmm") + std::to_string(reg);
            rs.frameOffset= (int32_t)(offsetSlots * 16u);
            rs.isXmm      = true;
            rs.xmmWidth   = 16;
            result.info.regSaves.push_back(rs);
            codeCur += 4; i += 2;
            break;
        }
        case UWOP_SAVE_XMM128_FAR: {
            uint32_t offsetBytes = view.readU32LE(codeCur + 2);
            RegSave rs;
            rs.regId      = 16 + reg;
            rs.regName    = std::string("xmm") + std::to_string(reg);
            rs.frameOffset= (int32_t)offsetBytes;
            rs.isXmm      = true;
            rs.xmmWidth   = 16;
            result.info.regSaves.push_back(rs);
            codeCur += 6; i += 3;
            break;
        }
        case UWOP_PUSH_MACH_FRAME:
            // Kernel mode; reg indicates error code present
            stackOffset += reg ? 48 : 40;
            codeCur += 2; i += 1;
            break;
        default:
            codeCur += 2; i += 1;
            break;
        }
    }

    // Align to DWORD after unwind codes
    uint64_t afterCodes = cur + (uint64_t)countOfCodes * 2;
    if (countOfCodes & 1) afterCodes += 2;  // padding

    if (result.hasChain) {
        result.chainRtFnVma = afterCodes;
        result.info.hasChainedUnwind = true;
        result.info.chainedInfoVma   = afterCodes;
    } else if (result.hasEHandler || result.hasUHandler) {
        // Exception handler RVA (DWORD) followed by handler data
        result.handlerDataVma = afterCodes + 4;
    }

    return result;
}

// ─── Parse FuncInfo / TryBlockMap ────────────────────────────────────────────

static void parseFuncInfo(const IBinaryView& view,
                           uint64_t base,
                           uint64_t funcInfoVma,
                           EHFunction& fn) {
    if (!view.isMapped(funcInfoVma)) return;

    uint32_t magic = view.readU32LE(funcInfoVma);
    // Supported magic numbers
    if (magic != 0x19930520 && magic != 0x19930521 && magic != 0x19930522) return;

    // DWORD maxState       = view.readU32LE(funcInfoVma + 4);
    // DWORD pUnwindMapRVA  = view.readU32LE(funcInfoVma + 8);
    uint32_t nTryBlocks     = view.readU32LE(funcInfoVma + 12);
    uint32_t pTryBlockRVA   = view.readU32LE(funcInfoVma + 16);

    if (nTryBlocks == 0 || pTryBlockRVA == 0) return;

    uint64_t tryBlockMapVma = base + pTryBlockRVA;

    for (uint32_t t = 0; t < nTryBlocks; ++t) {
        uint64_t entryVma = tryBlockMapVma + t * 20u;
        // int32_t tryLow   = view.readI32LE(entryVma + 0);
        // int32_t tryHigh  = view.readI32LE(entryVma + 4);
        // int32_t catchHigh= view.readI32LE(entryVma + 8);
        uint32_t nCatches   = view.readU32LE(entryVma + 12);
        uint32_t pHandlerRVA= view.readU32LE(entryVma + 16);

        if (nCatches == 0 || pHandlerRVA == 0) continue;

        TryCatchBlock block;
        // We set try ranges loosely here; the CFG structurer will tighten them
        // using the IpToStateMap later.  For now use the function's VMA range.
        block.tryBegin = fn.functionVma;
        block.tryEnd   = fn.functionEnd;

        uint64_t handlerArrVma = base + pHandlerRVA;
        for (uint32_t h = 0; h < nCatches; ++h) {
            uint64_t hVma = handlerArrVma + h * 16u;
            // uint32_t adjectives      = view.readU32LE(hVma + 0);
            uint32_t pTypeRVA        = view.readU32LE(hVma + 4);
            int32_t  dispCatchObj    = view.readI32LE(hVma + 8);
            uint32_t addrOfHandler   = view.readU32LE(hVma + 12);

            CatchHandler ch;
            ch.handlerVma    = base + addrOfHandler;
            ch.catchVarOffset= dispCatchObj;
            ch.isCatchAll    = (pTypeRVA == 0);

            if (!ch.isCatchAll && view.isMapped(base + pTypeRVA)) {
                // Dereference the TypeDescriptor pointer
                uint64_t typeDescVma = base + pTypeRVA;
                // Skip pVFTable (8) + spare (8) → name at +16
                uint64_t nameVma = typeDescVma + 16;
                // Read up to 256 chars of the mangled name
                char nameBuf[256] = {};
                view.readBytes(nameVma, reinterpret_cast<uint8_t*>(nameBuf),
                               sizeof(nameBuf) - 1);
                ch.catchType = demangleMsvcType(nameBuf);
            } else if (ch.isCatchAll) {
                ch.catchType = "...";
            }

            block.handlers.push_back(std::move(ch));
        }

        if (!block.handlers.empty()) {
            fn.tryCatchBlocks.push_back(std::move(block));
            fn.hasEH = true;
        }
    }
}

// ─── MSVC EH parser ───────────────────────────────────────────────────────────

class MsvcEHParser final : public IEHParser {
public:
    const char* name() const noexcept override { return "MSVC-SEH"; }

    std::vector<EHFunction> parse(const IBinaryView& view) const override {
        std::vector<EHFunction> results;

        uint64_t base      = view.imageBase();
        uint64_t excDirVma = view.sectionVma(".pdata");
        std::size_t excDirSize = view.sectionSize(".pdata");

        if (excDirVma == 0 || excDirSize == 0) return results;

        // Each RUNTIME_FUNCTION is 12 bytes
        std::size_t count = excDirSize / 12;

        for (std::size_t i = 0; i < count; ++i) {
            uint64_t rfVma = excDirVma + i * 12;

            uint32_t beginRVA  = view.readU32LE(rfVma + 0);
            uint32_t endRVA    = view.readU32LE(rfVma + 4);
            uint32_t unwindRVA = view.readU32LE(rfVma + 8);

            if (beginRVA == 0 || unwindRVA == 0) continue;

            uint64_t fnBegin   = base + beginRVA;
            uint64_t fnEnd     = base + endRVA;
            uint64_t unwindVma = base + unwindRVA;

            if (!view.isMapped(unwindVma)) continue;

            ParsedUnwindInfo parsed = parseUnwindInfo(view, base,
                                                       fnBegin, fnEnd,
                                                       unwindVma);

            EHFunction fn;
            fn.functionVma = fnBegin;
            fn.functionEnd = fnEnd;
            fn.unwindInfo  = parsed.info;

            // Follow chained unwind
            if (parsed.hasChain && view.isMapped(parsed.chainRtFnVma)) {
                uint32_t chainUnwindRVA = view.readU32LE(parsed.chainRtFnVma + 8);
                if (chainUnwindRVA) {
                    auto chained = parseUnwindInfo(view, base,
                                                   fnBegin, fnEnd,
                                                   base + chainUnwindRVA);
                    for (auto& rs : chained.info.regSaves)
                        fn.unwindInfo.regSaves.push_back(rs);
                }
            }

            // Parse C++ EH FuncInfo if present
            if ((parsed.hasEHandler || parsed.hasUHandler) &&
                parsed.handlerDataVma != 0 &&
                view.isMapped(parsed.handlerDataVma)) {

                // ExceptionHandler RVA is the DWORD just before handlerDataVma
                // The handler is usually __CxxFrameHandler3
                // FuncInfo comes right after that in the xdata section
                parseFuncInfo(view, base, parsed.handlerDataVma, fn);
            }

            results.push_back(std::move(fn));
        }

        return results;
    }
};

std::unique_ptr<IEHParser> makeMsvcEHParser() {
    return std::make_unique<MsvcEHParser>();
}

} // namespace eh_reconstruct
} // namespace retdec
