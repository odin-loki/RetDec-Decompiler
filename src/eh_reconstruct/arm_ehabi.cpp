/**
 * @file src/eh_reconstruct/arm_ehabi.cpp
 * @brief ARM EHABI (.ARM.exidx + .ARM.extab) exception handling parser.
 *
 * ## ARM Exception Handling ABI
 *
 * All ARM32 GCC/Clang toolchains use the EHABI format described in
 * "Exception Handling ABI for the ARM Architecture" (ARM IHI0038B).
 *
 * ### .ARM.exidx section
 *
 * One 8-byte entry per function, sorted in ascending function address order:
 *
 *   struct ExidxEntry {
 *     uint32_t prel31_fn;   // prel31 offset → function start VMA
 *     uint32_t data;        // one of three cases:
 *   };
 *
 *   Case 1: data == 0x00000001  → EXIDX_CANTUNWIND (no unwind info)
 *   Case 2: data bit31 == 1     → compact model 0 (Su16), unwind bytes in bits30..0
 *   Case 3: data bit31 == 0     → prel31 offset → .ARM.extab entry for this function
 *
 * ### .ARM.extab entry (generic / compact model 1 or 2)
 *
 *   Word 0:
 *     bits31..24 == 0x81 → compact model 1 (pr1); bits23..0 = first 3 unwind bytes
 *     bits31..24 == 0x82 → compact model 2 (pr2); same
 *     otherwise          → bits31..0 = prel31 offset to personality routine;
 *                          subsequent words are personality-specific data
 *
 *   For __gxx_personality_v0 (C++ EH):
 *     Word 1: bits31..28 = number of unwind words; bits27..24 = EH data words count
 *             bits23..0  = first 3 unwind bytes (if compact embedded)
 *   Then: unwind words
 *   Then: EH data (LSDA in Itanium format)
 *
 * ### Unwind byte opcodes (IHI0038B §9.3)
 *
 *   0x00–0x3F : vsp += (op + 1) * 4                           (Increment SP)
 *   0x40–0x7F : vsp -= (op - 0x3F) * 4                        (Decrement SP)
 *   0x80, 0x00: FINISH (op == 0x80 && next == 0x00)           (End of opcodes)
 *   0x80–0x8F + next byte : pop {r15..r12,r11..r4} bitmask    (Pop core regs)
 *   0x90–0x9F : if regN != r13 && regN != r15: vsp = regN     (Set vsp from reg)
 *   0xA0–0xA7 : pop {r4..r[4+op&7]}                           (Pop r4-rN)
 *   0xA8–0xAF : pop {r4..r[4+op&7], r14}                      (Pop r4-rN + LR)
 *   0xB0      : FINISH                                          (End of opcodes)
 *   0xB1 + next: pop low registers {r0..r3} bitmask
 *   0xB2 + uleb128: vsp += 0x204 + uleb128 * 4
 *   0xB3 + cd : save VFP (fstmfdx): d = low4(c) count = low4(d)+1; regs d[c..c+d]
 *   0xB4      : spare
 *   0xB5      : spare
 *   0xB6      : spare
 *   0xB7      : spare
 *   0xC0–0xC5 : WMMX wCGR regs (Intel Wireless MMX)
 *   0xC6 + cd : WMMX wR regs (similar to B3 encoding)
 *   0xC7 + next: WMMX wCGR bitmask
 *   0xC8–0xCF : VFP VPUSH {d[16+op&7]-d[...]} (FSTMFDX-style)
 *   0xD0–0xD7 : VFP VPUSH {d[4]-d[4+op&7]}    (FSTMFD-style)
 *   0xD8–0xFF : spare
 *
 * ### prel31 decoding
 *
 *   prel31 is a 31-bit two's complement value, sign-extended from bit30,
 *   added to the section VMA of the word that contains it:
 *
 *     int32_t offset = (int32_t)((raw << 1) >> 1); // sign extend from bit30
 *     uint64_t target = fieldVma + offset;
 */

#include <memory>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "retdec/eh_reconstruct/eh_reconstruct.h"
#include "lsda_parser.h"

namespace retdec {
namespace eh_reconstruct {

// ─── prel31 helper ───────────────────────────────────────────────────────────

static uint64_t decodePrel31(uint32_t raw, uint64_t fieldVma) {
    // Sign-extend from bit30
    int32_t offset = static_cast<int32_t>(raw << 1) >> 1;
    return static_cast<uint64_t>(static_cast<int64_t>(fieldVma) + offset);
}

// ─── ARM core register names ─────────────────────────────────────────────────

static const char* armCoreRegName(unsigned r) {
    static const char* names[] = {
        "r0","r1","r2","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12","sp","lr","pc"
    };
    if (r < 16) return names[r];
    return "unk";
}

// ─── Parse unwind byte stream ────────────────────────────────────────────────

/**
 * Parse ARM EHABI unwind byte opcodes into RegSave records and frame size.
 * `opcodes` is an array of bytes in order (already extracted from the entry).
 */
static void parseArmUnwindOpcodes(const std::vector<uint8_t>& opcodes,
                                   UnwindInfo& info) {
    std::size_t i = 0;
    int32_t vspOffset = 0;

    while (i < opcodes.size()) {
        uint8_t op = opcodes[i++];

        if (op <= 0x3F) {
            // vsp += (op+1)*4
            vspOffset += (op + 1) * 4;
        } else if (op <= 0x7F) {
            // vsp -= (op - 0x3F)*4
            vspOffset -= (int32_t)((op - 0x3F) * 4u);
        } else if (op == 0xB0) {
            // FINISH
            break;
        } else if (op == 0x80 && i < opcodes.size() && opcodes[i] == 0x00) {
            // FINISH (two-byte form)
            break;
        } else if (op >= 0x80 && op <= 0x8F) {
            // pop {r15..r12, r11..r4} via 12-bit bitmask
            if (i >= opcodes.size()) break;
            uint8_t next = opcodes[i++];
            uint16_t mask = (uint16_t)((op & 0x0F) << 8) | next;
            // bit N of mask → pop rN+4 (for low byte) or pop r12+N (for high nibble)
            // Actually: bits [11:8] from op nibble, bits [7:0] from next
            // bit15 → r15, bit14 → r14, ... bit4 → r4
            // mask bit interpretation per IHI0038B:
            // bit0 of next = r4, bit1=r5, ..., bit7=r11
            // bit0 of (op&0x0F) = r12, bit1=r13(sp, spare), bit2=r14, bit3=r15
            for (int r = 4; r <= 11; ++r) {
                if (mask & (1u << (r - 4))) {
                    RegSave rs;
                    rs.regId      = r;
                    rs.regName    = armCoreRegName(r);
                    rs.frameOffset= vspOffset;
                    rs.isXmm      = false;
                    info.regSaves.push_back(rs);
                }
            }
            // r12
            if (mask & (1u << 8)) {
                RegSave rs; rs.regId=12; rs.regName="r12"; rs.frameOffset=vspOffset; rs.isXmm=false;
                info.regSaves.push_back(rs);
            }
            // r14 (LR)
            if (mask & (1u << 10)) {
                RegSave rs; rs.regId=14; rs.regName="lr"; rs.frameOffset=vspOffset; rs.isXmm=false;
                info.regSaves.push_back(rs);
            }
            // r15 (PC) – saved return address
            if (mask & (1u << 11)) {
                RegSave rs; rs.regId=15; rs.regName="pc"; rs.frameOffset=vspOffset; rs.isXmm=false;
                info.regSaves.push_back(rs);
            }
        } else if (op >= 0x90 && op <= 0x9F) {
            // vsp = core register N (where N = op & 0xF, N != 13 or 15)
            // Records no register save (just sets the virtual SP)
        } else if (op >= 0xA0 && op <= 0xA7) {
            // pop {r4..r[4 + (op&7)]}
            int top = 4 + (op & 7);
            for (int r = 4; r <= top; ++r) {
                RegSave rs;
                rs.regId      = r;
                rs.regName    = armCoreRegName(r);
                rs.frameOffset= vspOffset;
                rs.isXmm      = false;
                info.regSaves.push_back(rs);
            }
        } else if (op >= 0xA8 && op <= 0xAF) {
            // pop {r4..r[4 + (op&7)], r14}
            int top = 4 + (op & 7);
            for (int r = 4; r <= top; ++r) {
                RegSave rs;
                rs.regId      = r;
                rs.regName    = armCoreRegName(r);
                rs.frameOffset= vspOffset;
                rs.isXmm      = false;
                info.regSaves.push_back(rs);
            }
            RegSave lr; lr.regId=14; lr.regName="lr"; lr.frameOffset=vspOffset; lr.isXmm=false;
            info.regSaves.push_back(lr);
        } else if (op == 0xB1) {
            if (i >= opcodes.size()) break;
            uint8_t mask = opcodes[i++];
            for (int r = 0; r <= 3; ++r) {
                if (mask & (1u << r)) {
                    RegSave rs; rs.regId=r; rs.regName=armCoreRegName(r);
                    rs.frameOffset=vspOffset; rs.isXmm=false;
                    info.regSaves.push_back(rs);
                }
            }
        } else if (op == 0xB2) {
            // vsp += 0x204 + uleb128*4
            uint32_t val = 0, shift = 0;
            while (i < opcodes.size()) {
                uint8_t b = opcodes[i++];
                val |= (uint32_t)(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
            }
            vspOffset += (int32_t)(0x204 + val * 4u);
        } else if (op == 0xB3) {
            // VFP fstmfdx: d[c]-d[c + (d_count)]
            if (i >= opcodes.size()) break;
            uint8_t cd = opcodes[i++];
            int dStart = (cd >> 4) & 0xF;
            int dCount = (cd & 0xF) + 1;
            for (int d = dStart; d < dStart + dCount; ++d) {
                RegSave rs;
                rs.regId      = 64 + d;  // VFP d-reg offset
                rs.regName    = "d" + std::to_string(d);
                rs.frameOffset= vspOffset;
                rs.isXmm      = true;
                rs.xmmWidth   = 8;  // 64-bit VFP double
                info.regSaves.push_back(rs);
            }
        } else if (op >= 0xC8 && op <= 0xCF) {
            // VPUSH {d[16 + (op & 7)]-d[16 + (op & 7) + ?]} — needs next byte
            if (i >= opcodes.size()) break;
            uint8_t cnt = opcodes[i++];
            int dStart = 16 + (op & 7);
            for (int d = dStart; d < dStart + (cnt & 0xF) + 1; ++d) {
                RegSave rs;
                rs.regId      = 64 + d;
                rs.regName    = "d" + std::to_string(d);
                rs.frameOffset= vspOffset;
                rs.isXmm      = true;
                rs.xmmWidth   = 8;
                info.regSaves.push_back(rs);
            }
        } else if (op >= 0xD0 && op <= 0xD7) {
            // VPUSH {d4-d[4 + (op & 7)]}
            int top = 4 + (op & 7);
            for (int d = 4; d <= top; ++d) {
                RegSave rs;
                rs.regId      = 64 + d;
                rs.regName    = "d" + std::to_string(d);
                rs.frameOffset= vspOffset;
                rs.isXmm      = true;
                rs.xmmWidth   = 8;
                info.regSaves.push_back(rs);
            }
        }
        // All other opcodes (WMMX, spare) → skip
    }

    // Unwind opcodes 0x00-0x3F increment VSP (reversing prologue SP decrement).
    // A positive vspOffset means the prologue decreased SP by that amount.
    info.frameSize = (uint32_t)(vspOffset > 0 ? vspOffset : -vspOffset);
}

// ─── Build EHFunction from EXIDX entry ───────────────────────────────────────

static std::optional<EHFunction> processExidxEntry(const IBinaryView& view,
                                                     uint64_t entryVma,
                                                     uint64_t nextFnVma) {
    uint32_t word0 = view.readU32LE(entryVma);
    uint32_t word1 = view.readU32LE(entryVma + 4);

    uint64_t fnStart = decodePrel31(word0, entryVma);
    uint64_t fnEnd   = nextFnVma ? nextFnVma : fnStart + 0x100;  // fallback

    EHFunction fn;
    fn.functionVma = fnStart;
    fn.functionEnd = fnEnd;
    fn.unwindInfo.functionBegin = fnStart;
    fn.unwindInfo.functionEnd   = fnEnd;

    // Case 1: CANTUNWIND
    if (word1 == 0x00000001) {
        return fn;  // no unwind info, but still a valid function entry
    }

    std::vector<uint8_t> opcodes;

    if (word1 & 0x80000000u) {
        // Case 2: compact model 0 (inline unwind)
        // Bits [23:16], [15:8], [7:0] are the three unwind bytes; bit31=1, bit30..24=0x80
        opcodes.push_back((word1 >> 16) & 0xFF);
        opcodes.push_back((word1 >>  8) & 0xFF);
        opcodes.push_back( word1        & 0xFF);
    } else {
        // Case 3: pointer to .ARM.extab
        uint64_t extabVma = decodePrel31(word1, entryVma + 4);
        if (!view.isMapped(extabVma)) return fn;

        uint32_t extabWord0 = view.readU32LE(extabVma);

        uint8_t model = (extabWord0 >> 24) & 0xFF;
        bool isCompact = (model == 0x81 || model == 0x82);

        if (isCompact) {
            // Compact model 1 or 2: inline opcodes in extab word0 + additional words
            int extraWords = (model == 0x81) ? ((extabWord0 >> 16) & 0xFF) : 0;
            // First 3 bytes from word0 bits[23:0]
            opcodes.push_back((extabWord0 >> 16) & 0xFF);
            opcodes.push_back((extabWord0 >>  8) & 0xFF);
            opcodes.push_back( extabWord0        & 0xFF);
            for (int w = 1; w <= extraWords && view.isMapped(extabVma + w*4); ++w) {
                uint32_t ew = view.readU32LE(extabVma + w * 4);
                opcodes.push_back((ew >> 24) & 0xFF);
                opcodes.push_back((ew >> 16) & 0xFF);
                opcodes.push_back((ew >>  8) & 0xFF);
                opcodes.push_back( ew        & 0xFF);
            }
        } else {
            // Generic model: extabWord0 is prel31 personality pointer
            fn.personalityFn = "personality@" + std::to_string(
                decodePrel31(extabWord0 & 0x7FFFFFFF, extabVma));

            // Word1: bits31..28 = nUnwindWords, bits27..24 = nEHDataWords
            if (!view.isMapped(extabVma + 4)) return fn;
            uint32_t word1ext = view.readU32LE(extabVma + 4);
            int nUnwindWords = (word1ext >> 28) & 0xF;
            int nEHDataWords = (word1ext >> 24) & 0xF;

            // First 3 bytes of unwind opcodes come from word1 bits[23:0]
            opcodes.push_back((word1ext >> 16) & 0xFF);
            opcodes.push_back((word1ext >>  8) & 0xFF);
            opcodes.push_back( word1ext        & 0xFF);

            // Additional unwind words
            for (int w = 2; w < 2 + nUnwindWords && view.isMapped(extabVma + w*4); ++w) {
                uint32_t ew = view.readU32LE(extabVma + w * 4);
                opcodes.push_back((ew >> 24) & 0xFF);
                opcodes.push_back((ew >> 16) & 0xFF);
                opcodes.push_back((ew >>  8) & 0xFF);
                opcodes.push_back( ew        & 0xFF);
            }

            // EH data follows the unwind words and is in Itanium-compatible LSDA
            // format (same as .gcc_except_table in .eh_frame-based binaries).
            if (nEHDataWords > 0) {
                // The LSDA starts at extabVma + (2 + nUnwindWords) * 4.
                uint64_t lsdaVma = extabVma + (2 + nUnwindWords) * 4;
                if (view.isMapped(lsdaVma)) {
                    auto lsda = parseLSDA(view, lsdaVma, fn.functionVma);
                    lsdaToTryCatch(view, lsda, fn);
                }
            }
        }
    }

    parseArmUnwindOpcodes(opcodes, fn.unwindInfo);
    return fn;
}

// ─── ARM EHABI parser ─────────────────────────────────────────────────────────

class ArmEhabiParser final : public IEHParser {
public:
    const char* name() const noexcept override { return "ARM-EHABI"; }

    std::vector<EHFunction> parse(const IBinaryView& view) const override {
        std::vector<EHFunction> results;

        uint64_t exidxVma  = view.sectionVma(".ARM.exidx");
        std::size_t exidxSz = view.sectionSize(".ARM.exidx");
        if (exidxVma == 0 || exidxSz < 8) return results;

        std::size_t count = exidxSz / 8;

        // Collect all function start VMAs for end-address calculation
        std::vector<uint64_t> fnStarts;
        fnStarts.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            uint32_t w0 = view.readU32LE(exidxVma + i * 8);
            fnStarts.push_back(decodePrel31(w0, exidxVma + i * 8));
        }

        for (std::size_t i = 0; i < count; ++i) {
            uint64_t entryVma  = exidxVma + i * 8;
            uint64_t nextFnVma = (i + 1 < count) ? fnStarts[i + 1] : 0;

            auto fn = processExidxEntry(view, entryVma, nextFnVma);
            if (fn) results.push_back(std::move(*fn));
        }

        return results;
    }
};

std::unique_ptr<IEHParser> makeArmEhabiParser() {
    return std::make_unique<ArmEhabiParser>();
}

} // namespace eh_reconstruct
} // namespace retdec
