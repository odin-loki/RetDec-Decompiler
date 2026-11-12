#pragma once
// Internal access to the generated UCD tables (ucd.cpp owns ucd_data.inc).
#include <forge/os/text.hpp>
#include <span>
namespace forge::os::text::detail {
struct props { unsigned char gc, script, gcb, wb, sb, lb, eaw, bidi, ccc, incb; unsigned flags; };
const props& prop(cp_t c);
enum flag : unsigned { F_Alpha = 1, F_Lower = 2, F_Upper = 4, F_Space = 8, F_DefIgn = 16, F_Cased = 32, F_CaseIgn = 64,
    F_Emoji = 128, F_EmojiPres = 256, F_ExtPict = 512, F_EmojiMod = 1024, F_EmojiModBase = 2048, F_Math = 4096,
    F_Hex = 8192, F_Dash = 16384, F_Ideo = 32768, F_NonChar = 65536, F_VS = 131072, F_UIdeo = 262144 };
// Enum orders match tools/unicode/gen_ucd.py.
enum gcb_t { GB_Other, GB_CR, GB_LF, GB_Control, GB_Extend, GB_ZWJ, GB_RI, GB_Prepend, GB_SpacingMark, GB_L, GB_V, GB_T, GB_LV, GB_LVT };
enum wb_t { WB_Other, WB_CR, WB_LF, WB_Newline, WB_Extend, WB_ZWJ, WB_RI, WB_Format, WB_Katakana, WB_Hebrew_Letter, WB_ALetter,
    WB_Single_Quote, WB_Double_Quote, WB_MidNumLet, WB_MidLetter, WB_MidNum, WB_Numeric, WB_ExtendNumLet, WB_WSegSpace };
enum sb_t { SB_Other, SB_CR, SB_LF, SB_Extend, SB_Sep, SB_Format, SB_Sp, SB_Lower, SB_Upper, SB_OLetter, SB_Numeric, SB_ATerm,
    SB_SContinue, SB_STerm, SB_Close };
enum lb_t { LB_XX, LB_BK, LB_CR, LB_LF, LB_CM, LB_NL, LB_SG, LB_WJ, LB_ZW, LB_GL, LB_SP, LB_ZWJ, LB_B2, LB_BA, LB_BB, LB_HY, LB_CB,
    LB_CL, LB_CP, LB_EX, LB_IN, LB_NS, LB_OP, LB_QU, LB_IS, LB_NU, LB_PO, LB_PR, LB_SY, LB_AI, LB_AL, LB_CJ, LB_EB, LB_EM, LB_H2,
    LB_H3, LB_HL, LB_ID, LB_JL, LB_JV, LB_JT, LB_RI, LB_SA, LB_AK, LB_AP, LB_AS, LB_VF, LB_VI };
enum eaw_t { EA_N, EA_A, EA_F, EA_H, EA_Na, EA_W };
enum bidi_t { B_L, B_R, B_AL, B_EN, B_ES, B_ET, B_AN, B_CS, B_NSM, B_BN, B_B, B_S, B_WS, B_ON, B_LRE, B_LRO, B_RLE, B_RLO, B_PDF,
    B_LRI, B_RLI, B_FSI, B_PDI };
enum incb_t { InCB_None, InCB_Linker, InCB_Consonant, InCB_Extend };

// Multi-code-point mappings: returns empty span if none.
std::span<const unsigned> special_upper(cp_t c);
std::span<const unsigned> special_lower(cp_t c);
std::span<const unsigned> special_title(cp_t c);
std::span<const unsigned> full_fold(cp_t c);
std::span<const unsigned> decomposition(cp_t c, bool& compat);   // one level
cp_t compose(cp_t a, cp_t b);                                     // 0 if none
cp_t bracket_pair(cp_t c, int& type);                             // type 1 open, 2 close, 0 none
// DUCET
struct ducet_view {
    std::span<const unsigned> key_o, key_pool, ce_o, ce_pool;
    struct implicit { unsigned lo, hi, base; };
    std::span<const implicit> implicits;
};
const ducet_view& ducet();
std::span<const char* const> script_names();
std::span<const char* const> gc_names();
std::span<const char* const> lb_names();
std::span<const char* const> flag_names();

// Small UTF-8 helpers used everywhere.
inline std::u32string to32(std::string_view s) { return utf8_to_utf32(s); }
// Code point offsets -> byte offsets for a UTF-8 string.
std::vector<std::size_t> cp_byte_offsets(std::string_view s);   // size = cps+1
}  // namespace forge::os::text::detail
