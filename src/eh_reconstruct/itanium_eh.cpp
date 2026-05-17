/**
 * @file src/eh_reconstruct/itanium_eh.cpp
 * @brief Itanium DWARF EH parser (.eh_frame + LSDA).
 *
 * .eh_frame format
 * ────────────────
 * The .eh_frame section is a sequence of records.  Each record starts with
 * a 4-byte length field (or 0xFFFFFFFF + 8-byte extended length).
 *
 * CIE (length != 0, CIE_id == 0):
 *   uint32_t  length
 *   uint32_t  CIE_id          (0 for CIE)
 *   uint8_t   version         (1 or 3)
 *   char[]    augmentation    (NUL-terminated: "zR", "zPR", "zRS", ...)
 *   uleb128   code_alignment
 *   sleb128   data_alignment
 *   uleb128   return_column
 *   uleb128   aug_data_len    (if 'z' in augmentation)
 *   -- augmentation data:
 *      'R' → uint8_t fde_pointer_encoding
 *      'P' → uint8_t personality_encoding; encoded_ptr personality
 *      'S' → signal frame (no data)
 *   CFA_instructions[]
 *
 * FDE (length != 0, CIE_id != 0):
 *   uint32_t  length
 *   int32_t   CIE_delta       (offset back to associated CIE)
 *   encoded   initial_location
 *   encoded   address_range
 *   uleb128   aug_data_len    (if 'z' in CIE augmentation)
 *   -- augmentation data:
 *      'L' → encoded_ptr  lsda_pointer
 *   CFA_instructions[]
 *
 * LSDA (Language Specific Data Area)
 * ─────────────────────────────────
 *   uint8_t   lpstart_enc     (DW_EH_PE_omit → use function start)
 *   [encoded] lpstart
 *   uint8_t   ttype_enc       (DW_EH_PE_omit → no type table)
 *   [uleb128] ttype_base_off  (offset from end of this field to ttype base)
 *   uint8_t   call_site_enc
 *   uleb128   call_site_table_len
 *   -- call site records (call_site_table_len bytes total):
 *      encoded  cs_start    (offset from lpstart)
 *      encoded  cs_len      (length of region)
 *      encoded  landing_pad (offset from lpstart; 0 = no handler)
 *      uleb128  action      (1-based index into action table; 0 = cleanup)
 *   -- action table (immediately after call site table):
 *      sleb128  type_filter  (>0 = type index; 0 = catch-all/cleanup; <0 = spec)
 *      sleb128  next         (offset to next action; 0 = end of chain)
 *   -- type table (grows downward from ttype_base):
 *      encoded  type_info_ptr  (index 1 at ttype_base-ptrSize)
 *
 * DW_EH_PE pointer encodings:
 *   lower nibble: DW_EH_PE_absptr=0, uleb128=1, udata2=2, udata4=3, udata8=4,
 *                 sleb128=9, sdata2=a, sdata4=b, sdata8=c
 *   upper nibble: DW_EH_PE_pcrel=0x10, DW_EH_PE_datarel=0x30,
 *                 DW_EH_PE_funcrel=0x40, DW_EH_PE_aligned=0x50
 *   0xFF = DW_EH_PE_omit
 */

#include <memory>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "retdec/eh_reconstruct/eh_reconstruct.h"
#include "lsda_parser.h"

namespace retdec {
namespace eh_reconstruct {

// ─── CIE state ────────────────────────────────────────────────────────────────

struct CIEInfo {
    uint64_t    cieVma       = 0;
    std::string augmentation;
    uint8_t     fdePointerEnc = DW_EH_PE_absptr;
    uint8_t     lsdaPointerEnc = DW_EH_PE_omit;
    uint64_t    personalityVma = 0;
    int64_t     dataAlignment = -8;  // x86-64 default
    uint64_t    codeAlignment = 1;
    uint32_t    returnColumn  = 16;  // RA on x86-64
};

// ─── .eh_frame parser ────────────────────────────────────────────────────────

class ItaniumEHParser final : public IEHParser {
public:
    const char* name() const noexcept override { return "Itanium-DWARF"; }

    std::vector<EHFunction> parse(const IBinaryView& view) const override {
        std::vector<EHFunction> results;

        uint64_t ehFrameVma  = view.sectionVma(".eh_frame");
        std::size_t ehFrameSz = view.sectionSize(".eh_frame");
        if (ehFrameVma == 0 || ehFrameSz == 0) return results;

        uint64_t end = ehFrameVma + ehFrameSz;
        uint64_t cur = ehFrameVma;

        // Map CIE offset → CIEInfo
        std::map<uint64_t, CIEInfo> cieMap;

        while (cur + 4 <= end) {
            uint64_t recordStart = cur;
            uint32_t length32 = view.readU32LE(cur); cur += 4;
            if (length32 == 0) break;  // terminator

            uint64_t recordLen;
            if (length32 == 0xFFFFFFFF) {
                recordLen = view.readU64LE(cur); cur += 8;
            } else {
                recordLen = length32;
            }

            // DWARF length field gives bytes AFTER itself (not including the 4-byte
            // length field). cur is already past the length field (or past the
            // 4+8-byte extended header for 64-bit DWARF), so recordEnd = cur + recordLen.
            uint64_t recordEnd = cur + recordLen;

            if (!view.isMapped(cur)) break;

            uint32_t cieId = view.readU32LE(cur); cur += 4;

            if (cieId == 0) {
                // ─── CIE ───
                CIEInfo cie;
                cie.cieVma = recordStart;

                uint8_t version = view.readU8(cur++);
                (void)version;

                // Read NUL-terminated augmentation string
                while (cur < recordEnd) {
                    uint8_t c = view.readU8(cur++);
                    if (c == 0) break;
                    cie.augmentation += (char)c;
                }

                cie.codeAlignment = view.readULEB128(cur);
                cie.dataAlignment = view.readSLEB128(cur);
                cie.returnColumn  = (uint32_t)view.readULEB128(cur);

                if (!cie.augmentation.empty() && cie.augmentation[0] == 'z') {
                    uint64_t augLen = view.readULEB128(cur);
                    uint64_t augEnd = cur + augLen;
                    for (std::size_t ai = 1; ai < cie.augmentation.size(); ++ai) {
                        char ac = cie.augmentation[ai];
                        if (ac == 'R') {
                            cie.fdePointerEnc = view.readU8(cur++);
                        } else if (ac == 'P') {
                            uint8_t pEnc = view.readU8(cur++);
                            cie.personalityVma = view.readEncodedPtr(cur, pEnc, cur);
                        } else if (ac == 'L') {
                            cie.lsdaPointerEnc = view.readU8(cur++);
                        } else if (ac == 'S') {
                            // signal frame, no data
                        }
                    }
                    cur = augEnd;
                }

                cieMap[recordStart] = cie;
            } else {
                // ─── FDE ───
                // CIE offset: the CIE is at (cur - 4 - cieId) in .eh_frame
                // cieId is the distance from the start of the cieId field back to the CIE
                uint64_t cieOff = (cur - 4) - cieId;

                auto it = cieMap.find(cieOff);
                if (it == cieMap.end()) {
                    cur = recordEnd;
                    continue;
                }
                const CIEInfo& cie = it->second;

                uint64_t funcStart = view.readEncodedPtr(cur, cie.fdePointerEnc, cur);
                uint64_t funcLen   = view.readEncodedPtr(cur, cie.fdePointerEnc & 0x0F, cur);
                uint64_t funcEnd   = funcStart + funcLen;

                uint64_t lsdaVma = 0;
                if (!cie.augmentation.empty() && cie.augmentation[0] == 'z') {
                    uint64_t augLen = view.readULEB128(cur);
                    uint64_t augEnd = cur + augLen;
                    if (cie.lsdaPointerEnc != DW_EH_PE_omit) {
                        lsdaVma = view.readEncodedPtr(cur, cie.lsdaPointerEnc, cur);
                    }
                    cur = augEnd;
                }

                // ── Parse CFA instructions for register-save locations ──────
                // DWARF §6.4.1: CFA instructions live between cur and recordEnd.
                // We extract register save offsets (DW_CFA_offset / DW_CFA_offset_extended)
                // and CFA definition (DW_CFA_def_cfa / DW_CFA_def_cfa_offset) to
                // populate UnwindInfo.
                struct CfaState {
                    uint32_t cfaReg    = 7;   // RSP by default (x86-64)
                    int64_t  cfaOffset = 8;   // return address already pushed
                    std::vector<RegSave> saves;
                };
                auto parseCFAInstrs = [&](uint64_t from, uint64_t to,
                                          const CIEInfo& c) -> CfaState
                {
                    CfaState st;
                    // x86-64 named registers for human-readable output
                    static const char* kRegNames64[] = {
                        "rax","rdx","rcx","rbx","rsi","rdi","rbp","rsp",
                        "r8","r9","r10","r11","r12","r13","r14","r15","rip"
                    };
                    auto regName = [](uint32_t r) -> std::string {
                        if (r < 17) return kRegNames64[r];
                        return "reg" + std::to_string(r);
                    };
                    uint64_t p = from;
                    while (p < to) {
                        uint8_t op = view.readU8(p++);
                        uint8_t hi = op >> 6;
                        uint8_t lo = op & 0x3F;
                        if (hi == 1) {
                            // DW_CFA_advance_loc — skip
                        } else if (hi == 2) {
                            // DW_CFA_offset reg — reg saved at cfa + offset*dataAlign
                            uint64_t off = view.readULEB128(p);
                            RegSave rs;
                            rs.regId       = lo;
                            rs.regName     = regName(lo);
                            rs.frameOffset = static_cast<int32_t>(
                                static_cast<int64_t>(off) * c.dataAlignment);
                            st.saves.push_back(rs);
                        } else if (hi == 3) {
                            // DW_CFA_restore — nothing to store
                        } else switch (op) {
                            case 0x00: break;  // DW_CFA_nop
                            case 0x0C: {       // DW_CFA_def_cfa reg, off
                                st.cfaReg    = static_cast<uint32_t>(view.readULEB128(p));
                                st.cfaOffset = static_cast<int64_t>(view.readULEB128(p));
                                break;
                            }
                            case 0x0D: {       // DW_CFA_def_cfa_register reg
                                st.cfaReg = static_cast<uint32_t>(view.readULEB128(p));
                                break;
                            }
                            case 0x0E: {       // DW_CFA_def_cfa_offset off
                                st.cfaOffset = static_cast<int64_t>(view.readULEB128(p));
                                break;
                            }
                            case 0x11: {       // DW_CFA_offset_extended reg, off
                                uint32_t r   = static_cast<uint32_t>(view.readULEB128(p));
                                uint64_t off = view.readULEB128(p);
                                RegSave rs;
                                rs.regId       = r;
                                rs.regName     = regName(r);
                                rs.frameOffset = static_cast<int32_t>(
                                    static_cast<int64_t>(off) * c.dataAlignment);
                                st.saves.push_back(rs);
                                break;
                            }
                            case 0x14: {       // DW_CFA_def_cfa_sf reg, off_sf
                                st.cfaReg    = static_cast<uint32_t>(view.readULEB128(p));
                                st.cfaOffset = view.readSLEB128(p) *
                                               static_cast<int64_t>(c.dataAlignment);
                                break;
                            }
                            case 0x15: {       // DW_CFA_def_cfa_offset_sf off_sf
                                st.cfaOffset = view.readSLEB128(p) *
                                               static_cast<int64_t>(c.dataAlignment);
                                break;
                            }
                            default: {
                                // Unknown opcode — stop parsing to avoid mis-sync.
                                p = to;
                                break;
                            }
                        }
                    }
                    return st;
                };
                CfaState cfaSt = parseCFAInstrs(cur, recordEnd, cie);

                EHFunction fn;
                fn.functionVma = funcStart;
                fn.functionEnd = funcEnd;
                fn.unwindInfo.functionBegin = funcStart;
                fn.unwindInfo.functionEnd   = funcEnd;
                fn.unwindInfo.frameReg      = cfaSt.cfaReg;
                fn.unwindInfo.frameRegOffset= static_cast<uint32_t>(
                    cfaSt.cfaOffset >= 0 ? cfaSt.cfaOffset : 0);
                fn.unwindInfo.frameSize     = static_cast<uint32_t>(
                    cfaSt.cfaOffset > 0 ? cfaSt.cfaOffset : 0);
                fn.unwindInfo.regSaves      = std::move(cfaSt.saves);
                fn.personalityFn = cie.personalityVma ? "personality@" +
                    std::to_string(cie.personalityVma) : "";

                if (lsdaVma != 0 && view.isMapped(lsdaVma)) {
                    auto lsda = parseLSDA(view, lsdaVma, funcStart);
                    lsdaToTryCatch(view, lsda, fn);
                }

                results.push_back(std::move(fn));
            }

            cur = recordEnd;
        }

        return results;
    }
};

std::unique_ptr<IEHParser> makeItaniumEHParser() {
    return std::make_unique<ItaniumEHParser>();
}

} // namespace eh_reconstruct
} // namespace retdec
