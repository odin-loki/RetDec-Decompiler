#include "fuse/text/text.hpp"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#if FUSE_TEXT_FULL
#include "text_full.hpp"
#endif

namespace fuse::text {

TextProfile textProfile() {
#if FUSE_TEXT_FULL
    return TextProfile::Full;
#else
    return TextProfile::Minimal;
#endif
}
const char* textProfileName(TextProfile p) {
    switch (p) {
    case TextProfile::Minimal: return "MINIMAL";
    case TextProfile::Full: return "FULL";
    case TextProfile::Cooked: return "COOKED";
    }
    return "?";
}
const char* textErrorName(TextError e) {
    switch (e) {
    case TextError::InvalidFont: return "invalid_font";
    case TextError::NoGlyphs: return "no_glyphs";
    case TextError::AtlasFull: return "atlas_full";
    case TextError::InvalidUtf8: return "invalid_utf8";
    }
    return "?";
}

// ---- UTF-8 ---------------------------------------------------------------------------------------------------------
char32_t decodeUtf8(std::string_view s, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) -> int {
        if (i + k >= s.size()) return -1;
        const auto b = static_cast<unsigned char>(s[i + k]);
        return (b & 0xC0u) == 0x80u ? int(b & 0x3Fu) : -1;
    };
    if (b0 < 0x80u) { ++i; return b0; }
    int n = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0u) == 0xC0u) { n = 1; cp = b0 & 0x1Fu; }
    else if ((b0 & 0xF0u) == 0xE0u) { n = 2; cp = b0 & 0x0Fu; }
    else if ((b0 & 0xF8u) == 0xF0u) { n = 3; cp = b0 & 0x07u; }
    else { ++i; return 0xFFFD; }
    for (int k = 1; k <= n; ++k) {
        const int c = cont(std::size_t(k));
        if (c < 0) { ++i; return 0xFFFD; }
        cp = (cp << 6) | char32_t(c);
    }
    i += std::size_t(n) + 1;
    return cp;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) out.push_back(char(cp));
    else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

std::size_t prevCodepoint(std::string_view s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) --i;
    return i;
}
std::size_t nextCodepoint(std::string_view s, std::size_t i) {
    if (i >= s.size()) return s.size();
    decodeUtf8(s, i);
    return i;
}

// ---- UAX #29 grapheme clusters -------------------------------------------------------------------------------------
namespace {
struct CpRange { char32_t lo, hi; };
template <std::size_t N> bool inRanges(const CpRange (&r)[N], char32_t c) {
    std::size_t a = 0, b = N;
    while (a < b) {
        const std::size_t m = (a + b) / 2;
        if (c < r[m].lo) b = m;
        else if (c > r[m].hi) a = m + 1;
        else return true;
    }
    return false;
}
// Grapheme_Cluster_Break=Extend (main blocks; sorted)
constexpr CpRange kExtend[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5},
    {0x05C7, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x07EB, 0x07F3},
    {0x0816, 0x0819}, {0x081B, 0x0823}, {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x0898, 0x089F},
    {0x08CA, 0x08E1}, {0x08E3, 0x0902}, {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D},
    {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0981, 0x0981}, {0x09BC, 0x09BC}, {0x09BE, 0x09BE}, {0x09C1, 0x09C4},
    {0x09CD, 0x09CD}, {0x09D7, 0x09D7}, {0x09E2, 0x09E3}, {0x0A01, 0x0A02}, {0x0A3C, 0x0A3C}, {0x0A41, 0x0A51},
    {0x0A70, 0x0A71}, {0x0A75, 0x0A75}, {0x0A81, 0x0A82}, {0x0ABC, 0x0ABC}, {0x0AC1, 0x0AC8}, {0x0ACD, 0x0ACD},
    {0x0AE2, 0x0AE3}, {0x0B01, 0x0B01}, {0x0B3C, 0x0B3C}, {0x0B3E, 0x0B3F}, {0x0B41, 0x0B44}, {0x0B4D, 0x0B4D},
    {0x0B55, 0x0B57}, {0x0B62, 0x0B63}, {0x0B82, 0x0B82}, {0x0BBE, 0x0BBE}, {0x0BC0, 0x0BC0}, {0x0BCD, 0x0BCD},
    {0x0BD7, 0x0BD7}, {0x0C00, 0x0C00}, {0x0C04, 0x0C04}, {0x0C3C, 0x0C3C}, {0x0C3E, 0x0C40}, {0x0C46, 0x0C56},
    {0x0C62, 0x0C63}, {0x0C81, 0x0C81}, {0x0CBC, 0x0CBC}, {0x0CBF, 0x0CBF}, {0x0CC2, 0x0CC2}, {0x0CC6, 0x0CC6},
    {0x0CCC, 0x0CCD}, {0x0CD5, 0x0CD6}, {0x0CE2, 0x0CE3}, {0x0D00, 0x0D01}, {0x0D3B, 0x0D3C}, {0x0D3E, 0x0D3E},
    {0x0D41, 0x0D44}, {0x0D4D, 0x0D4D}, {0x0D57, 0x0D57}, {0x0D62, 0x0D63}, {0x0D81, 0x0D81}, {0x0DCA, 0x0DCA},
    {0x0DCF, 0x0DCF}, {0x0DD2, 0x0DD6}, {0x0DDF, 0x0DDF}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC}, {0x0EC8, 0x0ECE}, {0x0F18, 0x0F19}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37},
    {0x0F39, 0x0F39}, {0x0F71, 0x0F7E}, {0x0F80, 0x0F84}, {0x0F86, 0x0F87}, {0x0F8D, 0x0FBC}, {0x0FC6, 0x0FC6},
    {0x102D, 0x1030}, {0x1032, 0x1037}, {0x1039, 0x103A}, {0x103D, 0x103E}, {0x1058, 0x1059}, {0x105E, 0x1060},
    {0x1071, 0x1074}, {0x1082, 0x1082}, {0x1085, 0x1086}, {0x108D, 0x108D}, {0x109D, 0x109D}, {0x135D, 0x135F},
    {0x1712, 0x1714}, {0x1732, 0x1733}, {0x1752, 0x1753}, {0x1772, 0x1773}, {0x17B4, 0x17B5}, {0x17B7, 0x17BD},
    {0x17C6, 0x17C6}, {0x17C9, 0x17D3}, {0x17DD, 0x17DD}, {0x180B, 0x180D}, {0x180F, 0x180F}, {0x1885, 0x1886},
    {0x18A9, 0x18A9}, {0x1920, 0x1922}, {0x1927, 0x1928}, {0x1932, 0x1932}, {0x1939, 0x193B}, {0x1A17, 0x1A18},
    {0x1A1B, 0x1A1B}, {0x1A56, 0x1A56}, {0x1A58, 0x1A60}, {0x1A62, 0x1A62}, {0x1A65, 0x1A6C}, {0x1A73, 0x1A7F},
    {0x1AB0, 0x1ACE}, {0x1B00, 0x1B03}, {0x1B34, 0x1B3D}, {0x1B42, 0x1B44}, {0x1B6B, 0x1B73}, {0x1B80, 0x1B81},
    {0x1BA2, 0x1BA5}, {0x1BA8, 0x1BAD}, {0x1BE6, 0x1BE6}, {0x1BE8, 0x1BE9}, {0x1BED, 0x1BED}, {0x1BEF, 0x1BF3},
    {0x1C2C, 0x1C33}, {0x1C36, 0x1C37}, {0x1CD0, 0x1CD2}, {0x1CD4, 0x1CE0}, {0x1CE2, 0x1CE8}, {0x1CED, 0x1CED},
    {0x1CF4, 0x1CF4}, {0x1CF8, 0x1CF9}, {0x1DC0, 0x1DFF}, {0x200C, 0x200C}, {0x20D0, 0x20F0}, {0x2CEF, 0x2CF1},
    {0x2D7F, 0x2D7F}, {0x2DE0, 0x2DFF}, {0x302A, 0x302F}, {0x3099, 0x309A}, {0xA66F, 0xA672}, {0xA674, 0xA67D},
    {0xA69E, 0xA69F}, {0xA6F0, 0xA6F1}, {0xA802, 0xA802}, {0xA806, 0xA806}, {0xA80B, 0xA80B}, {0xA825, 0xA826},
    {0xA82C, 0xA82C}, {0xA8C4, 0xA8C5}, {0xA8E0, 0xA8F1}, {0xA8FF, 0xA8FF}, {0xA926, 0xA92D}, {0xA947, 0xA951},
    {0xA980, 0xA982}, {0xA9B3, 0xA9B3}, {0xA9B6, 0xA9B9}, {0xA9BC, 0xA9BD}, {0xA9E5, 0xA9E5}, {0xAA29, 0xAA2E},
    {0xAA31, 0xAA32}, {0xAA35, 0xAA36}, {0xAA43, 0xAA43}, {0xAA4C, 0xAA4C}, {0xAA7C, 0xAA7C}, {0xAAB0, 0xAAB0},
    {0xAAB2, 0xAAB4}, {0xAAB7, 0xAAB8}, {0xAABE, 0xAABF}, {0xAAC1, 0xAAC1}, {0xAAEC, 0xAAED}, {0xAAF6, 0xAAF6},
    {0xABE5, 0xABE5}, {0xABE8, 0xABE8}, {0xABED, 0xABED}, {0xFB1E, 0xFB1E}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    {0xFF9E, 0xFF9F}, {0x101FD, 0x101FD}, {0x102E0, 0x102E0}, {0x10376, 0x1037A}, {0x10A01, 0x10A0F},
    {0x10A38, 0x10A3F}, {0x10AE5, 0x10AE6}, {0x10D24, 0x10D27}, {0x10EAB, 0x10EAC}, {0x10F46, 0x10F50},
    {0x11001, 0x11001}, {0x11038, 0x11046}, {0x1107F, 0x11081}, {0x110B3, 0x110B6}, {0x110B9, 0x110BA},
    {0x11100, 0x11102}, {0x11127, 0x1112B}, {0x1112D, 0x11134}, {0x1D165, 0x1D165}, {0x1D167, 0x1D169},
    {0x1D16E, 0x1D172}, {0x1D17B, 0x1D182}, {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD}, {0x1E8D0, 0x1E8D6},
    {0x1E944, 0x1E94A}, {0x1F3FB, 0x1F3FF}, {0xE0020, 0xE007F}, {0xE0100, 0xE01EF},
};
constexpr CpRange kSpacingMark[] = {
    {0x0903, 0x0903}, {0x093B, 0x093B}, {0x093E, 0x0940}, {0x0949, 0x094C}, {0x094E, 0x094F}, {0x0982, 0x0983},
    {0x09BF, 0x09C0}, {0x09C7, 0x09C8}, {0x09CB, 0x09CC}, {0x0A03, 0x0A03}, {0x0A3E, 0x0A40}, {0x0A83, 0x0A83},
    {0x0ABE, 0x0AC0}, {0x0AC9, 0x0AC9}, {0x0ACB, 0x0ACC}, {0x0B02, 0x0B03}, {0x0B40, 0x0B40}, {0x0B47, 0x0B48},
    {0x0B4B, 0x0B4C}, {0x0BBF, 0x0BBF}, {0x0BC1, 0x0BC2}, {0x0BC6, 0x0BC8}, {0x0BCA, 0x0BCC}, {0x0C01, 0x0C03},
    {0x0C41, 0x0C44}, {0x0C82, 0x0C83}, {0x0CBE, 0x0CBE}, {0x0CC0, 0x0CC1}, {0x0CC3, 0x0CC4}, {0x0CC7, 0x0CC8},
    {0x0CCA, 0x0CCB}, {0x0D02, 0x0D03}, {0x0D3F, 0x0D40}, {0x0D46, 0x0D48}, {0x0D4A, 0x0D4C}, {0x0D82, 0x0D83},
    {0x0DD0, 0x0DD1}, {0x0DD8, 0x0DDE}, {0x0DF2, 0x0DF3}, {0x0E33, 0x0E33}, {0x0EB3, 0x0EB3}, {0x0F3E, 0x0F3F},
    {0x0F7F, 0x0F7F}, {0x1031, 0x1031}, {0x103B, 0x103C}, {0x1056, 0x1057}, {0x1084, 0x1084}, {0x17B6, 0x17B6},
    {0x17BE, 0x17C5}, {0x17C7, 0x17C8}, {0x1923, 0x1926}, {0x1929, 0x192B}, {0x1930, 0x1931}, {0x1933, 0x1938},
    {0x1A19, 0x1A1A}, {0x1A55, 0x1A55}, {0x1A57, 0x1A57}, {0x1A6D, 0x1A72}, {0x1B04, 0x1B04}, {0x1B3E, 0x1B41},
    {0x1B82, 0x1B82}, {0x1BA1, 0x1BA1}, {0x1BA6, 0x1BA7}, {0x1BE7, 0x1BE7}, {0x1BEA, 0x1BEC}, {0x1BEE, 0x1BEE},
    {0x1C24, 0x1C2B}, {0x1C34, 0x1C35}, {0x1CE1, 0x1CE1}, {0x1CF7, 0x1CF7}, {0xA823, 0xA824}, {0xA827, 0xA827},
    {0xA880, 0xA881}, {0xA8B4, 0xA8C3}, {0xA952, 0xA952}, {0xA983, 0xA983}, {0xA9B4, 0xA9B5}, {0xA9BA, 0xA9BB},
    {0xA9BE, 0xA9BF}, {0xAA2F, 0xAA30}, {0xAA33, 0xAA34}, {0xAA4D, 0xAA4D}, {0xAAEB, 0xAAEB}, {0xAAEE, 0xAAEF},
    {0xAAF5, 0xAAF5}, {0xABE3, 0xABE4}, {0xABE6, 0xABE7}, {0xABE9, 0xABEA}, {0xABEC, 0xABEC},
};
constexpr CpRange kPrepend[] = {
    {0x0600, 0x0605}, {0x06DD, 0x06DD}, {0x070F, 0x070F}, {0x0890, 0x0891}, {0x08E2, 0x08E2}, {0x0D4E, 0x0D4E},
    {0x110BD, 0x110BD}, {0x110CD, 0x110CD}, {0x111C2, 0x111C3}, {0x1193F, 0x1193F}, {0x11941, 0x11941},
    {0x11A3A, 0x11A3A}, {0x11A84, 0x11A89}, {0x11D46, 0x11D46},
};
constexpr CpRange kExtPict[] = {
    {0x00A9, 0x00A9}, {0x00AE, 0x00AE}, {0x203C, 0x203C}, {0x2049, 0x2049}, {0x2122, 0x2122}, {0x2139, 0x2139},
    {0x2194, 0x2199}, {0x21A9, 0x21AA}, {0x231A, 0x231B}, {0x2328, 0x2328}, {0x2388, 0x2388}, {0x23CF, 0x23CF},
    {0x23E9, 0x23F3}, {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB}, {0x25B6, 0x25B6}, {0x25C0, 0x25C0},
    {0x25FB, 0x25FE}, {0x2600, 0x2605}, {0x2607, 0x2612}, {0x2614, 0x2685}, {0x2690, 0x2705}, {0x2708, 0x2712},
    {0x2714, 0x2714}, {0x2716, 0x2716}, {0x271D, 0x271D}, {0x2721, 0x2721}, {0x2728, 0x2728}, {0x2733, 0x2734},
    {0x2744, 0x2744}, {0x2747, 0x2747}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757},
    {0x2763, 0x2767}, {0x2795, 0x2797}, {0x27A1, 0x27A1}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2934, 0x2935},
    {0x2B05, 0x2B07}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x3030, 0x3030}, {0x303D, 0x303D},
    {0x3297, 0x3297}, {0x3299, 0x3299}, {0x1F000, 0x1F0FF}, {0x1F10D, 0x1F10F}, {0x1F12F, 0x1F12F},
    {0x1F16C, 0x1F171}, {0x1F17E, 0x1F17F}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1AD, 0x1F1E5},
    {0x1F201, 0x1F20F}, {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A}, {0x1F23C, 0x1F23F},
    {0x1F249, 0x1F3FA}, {0x1F400, 0x1F53D}, {0x1F546, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F774, 0x1F77F},
    {0x1F7D5, 0x1F7FF}, {0x1F80C, 0x1F80F}, {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F}, {0x1F888, 0x1F88F},
    {0x1F8AE, 0x1F8FF}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1FAFF}, {0x1FC00, 0x1FFFD},
};
using GB = GraphemeBreak;
struct ClusterState {
    GB prev = GB::Control;
    bool first = true;
    std::uint32_t riRun = 0;        // regional indicators since the last non-RI
    bool pictSeq = false; // ExtPict Extend* seen (GB11 left side)
    bool pictZwj = false; // ... followed by ZWJ
    std::uint8_t incb = 0; // GB9c: 1 after InCB=Consonant [Extend Linker]*, 2 once a Linker was seen
};
// Indic_Conjunct_Break (Unicode 15.1, DerivedCoreProperties): Linkers are the viramas of Devanagari, Bengali,
// Gujarati, Oriya, Telugu and Malayalam; Consonants are those scripts' consonant letters.
bool incbLinker(char32_t c) {
    return c == 0x094D || c == 0x09CD || c == 0x0ACD || c == 0x0B4D || c == 0x0C4D || c == 0x0D4D;
}
constexpr CpRange kIncbConsonant[] = {
    {0x0915, 0x0939}, {0x0958, 0x095F}, {0x0978, 0x097F}, {0x0995, 0x09A8}, {0x09AA, 0x09B0}, {0x09B2, 0x09B2},
    {0x09B6, 0x09B9}, {0x09DC, 0x09DD}, {0x09DF, 0x09DF}, {0x09F0, 0x09F1}, {0x0A95, 0x0AA8}, {0x0AAA, 0x0AB0},
    {0x0AB2, 0x0AB3}, {0x0AB5, 0x0AB9}, {0x0AF9, 0x0AF9}, {0x0B15, 0x0B28}, {0x0B2A, 0x0B30}, {0x0B32, 0x0B33},
    {0x0B35, 0x0B39}, {0x0B5C, 0x0B5D}, {0x0B5F, 0x0B5F}, {0x0B71, 0x0B71}, {0x0C15, 0x0C28}, {0x0C2A, 0x0C39},
    {0x0C58, 0x0C5A}, {0x0D15, 0x0D3A},
};
// true when there is a boundary before `cur` given the state after the previous code point
bool breakBefore(const ClusterState& st, GB cur, char32_t cp) {
    if (st.first) return true;                                         // GB1
    const GB p = st.prev;
    if (p == GB::CR && cur == GB::LF) return false;                     // GB3
    if (p == GB::Control || p == GB::CR || p == GB::LF) return true;    // GB4
    if (cur == GB::Control || cur == GB::CR || cur == GB::LF) return true; // GB5
    if (p == GB::L && (cur == GB::L || cur == GB::V || cur == GB::LV || cur == GB::LVT)) return false; // GB6
    if ((p == GB::LV || p == GB::V) && (cur == GB::V || cur == GB::T)) return false;                   // GB7
    if ((p == GB::LVT || p == GB::T) && cur == GB::T) return false;                                    // GB8
    if (cur == GB::Extend || cur == GB::ZWJ) return false;             // GB9
    if (cur == GB::SpacingMark) return false;                          // GB9a
    if (p == GB::Prepend) return false;                                // GB9b
    if (st.incb == 2 && inRanges(kIncbConsonant, cp)) return false;     // GB9c
    if (p == GB::ZWJ && cur == GB::ExtPict && st.pictZwj) return false; // GB11
    if (p == GB::RegionalIndicator && cur == GB::RegionalIndicator && (st.riRun % 2) == 1) return false; // GB12/13
    return true;                                                       // GB999
}
void advance(ClusterState& st, GB cur, char32_t cp) {
    if (inRanges(kIncbConsonant, cp)) st.incb = 1;
    else if (st.incb && incbLinker(cp)) st.incb = 2;
    else if (!(st.incb && (cur == GB::Extend || cur == GB::ZWJ))) st.incb = 0;
    st.riRun = cur == GB::RegionalIndicator ? st.riRun + 1 : 0;
    if (cur == GB::ExtPict) { st.pictSeq = true; st.pictZwj = false; }
    else if (cur == GB::Extend && st.pictSeq) {}
    else if (cur == GB::ZWJ && st.pictSeq) { st.pictZwj = true; st.pictSeq = false; }
    else { st.pictSeq = false; st.pictZwj = false; }
    st.prev = cur;
    st.first = false;
}
// Grapheme boundaries never cross a line feed, so scanning restarts after the previous '\n'.
std::size_t lineStart(std::string_view s, std::size_t i) {
    while (i > 0 && s[i - 1] != '\n') --i;
    return i;
}
} // namespace

GraphemeBreak graphemeBreakProperty(char32_t c) {
    if (c == '\r') return GB::CR;
    if (c == '\n') return GB::LF;
    if (c == 0x200D) return GB::ZWJ;
    if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x00AD) return GB::Control;
    if (c == 0x200B || c == 0x2028 || c == 0x2029 || c == 0xFEFF || (c >= 0x200E && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
        (c >= 0x2060 && c <= 0x206F) || (c >= 0xFFF0 && c <= 0xFFFB) || (c >= 0xE0000 && c <= 0xE001F))
        return GB::Control;
    if (c < 0x0300) return (c == 0xA9 || c == 0xAE) ? GB::ExtPict : GB::Other;
    if (c >= 0x1F1E6 && c <= 0x1F1FF) return GB::RegionalIndicator;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0xA960 && c <= 0xA97C)) return GB::L;
    if ((c >= 0x1160 && c <= 0x11A7) || (c >= 0xD7B0 && c <= 0xD7C6)) return GB::V;
    if ((c >= 0x11A8 && c <= 0x11FF) || (c >= 0xD7CB && c <= 0xD7FB)) return GB::T;
    if (c >= 0xAC00 && c <= 0xD7A3) return (c - 0xAC00) % 28 == 0 ? GB::LV : GB::LVT;
    if (inRanges(kExtend, c)) return GB::Extend;
    if (inRanges(kSpacingMark, c)) return GB::SpacingMark;
    if (inRanges(kPrepend, c)) return GB::Prepend;
    if (inRanges(kExtPict, c)) return GB::ExtPict;
    return GB::Other;
}

std::size_t nextBoundary(std::string_view s, std::size_t i) {
    if (i >= s.size()) return s.size();
    // walk from the line start so the RI parity and the emoji ZWJ state are right
    ClusterState st;
    std::size_t at = lineStart(s, i);
    while (at < s.size()) {
        std::size_t nx = at;
        const char32_t cp = decodeUtf8(s, nx);
        const GB g = graphemeBreakProperty(cp);
        if (at > i && breakBefore(st, g, cp)) return at;
        advance(st, g, cp);
        at = nx;
    }
    return s.size();
}

std::size_t prevBoundary(std::string_view s, std::size_t i) {
    if (i == 0) return 0;
    if (i > s.size()) i = s.size();
    // CR LF is one cluster: from after it, step back over both
    if (s[i - 1] == '\n') return (i >= 2 && s[i - 2] == '\r') ? i - 2 : i - 1;
    ClusterState st;
    std::size_t at = lineStart(s, i - 1), last = at;
    while (at < i) {
        std::size_t nx = at;
        const char32_t cp = decodeUtf8(s, nx);
        const GB g = graphemeBreakProperty(cp);
        if (breakBefore(st, g, cp)) last = at;
        advance(st, g, cp);
        at = nx;
    }
    return last;
}

std::size_t graphemeCount(std::string_view s) {
    std::size_t n = 0;
    ClusterState st;
    for (std::size_t at = 0; at < s.size();) {
        const char32_t cp = decodeUtf8(s, at);
        const GB g = graphemeBreakProperty(cp);
        if (breakBefore(st, g, cp)) ++n;
        advance(st, g, cp);
    }
    return n;
}

// ---- Font ----------------------------------------------------------------------------------------------------------
struct Font::State {
    std::vector<std::uint8_t> bytes;
    stbtt_fontinfo info{};
    float unitsPerPixelScale1 = 0.f; // stbtt_ScaleForPixelHeight(info, 1)
    float emPerPixelSize = 1.f;      // em size in pixels for pixelSize 1 (pixel sizes are ascent-to-descent)
    std::vector<const Font*> fallbacks;
    std::vector<ColorLayer> layers;                       // COLRv0 layer records with CPAL palette 0 resolved
    std::vector<std::array<std::uint32_t, 3>> colorBase; // (glyph, first layer, layer count), sorted by glyph
#if FUSE_TEXT_FULL
    detail::FullFace* full = nullptr; // owned; HarfBuzz + FreeType over `bytes`
    ~State() { detail::fullDestroy(full); }
#endif
};

namespace {
std::atomic<std::uint32_t> g_fontIds{1};

std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }

// COLR version 0 + CPAL palette 0. Bounds-checked: a malformed table leaves the font without colour glyphs.
void loadColr(std::vector<std::uint8_t>& bytes, std::vector<ColorLayer>& layers,
              std::vector<std::array<std::uint32_t, 3>>& colorBase, std::uint32_t fontStart) {
    std::uint8_t* d = bytes.data();
    const std::size_t n = bytes.size();
    const std::uint32_t colr = stbtt__find_table(d, fontStart, "COLR");
    const std::uint32_t cpal = stbtt__find_table(d, fontStart, "CPAL");
    if (!colr || !cpal || colr + 14 > n || cpal + 12 > n) return;
    const std::uint16_t numBase = be16(d + colr + 2), numLayers = be16(d + colr + 12);
    const std::uint32_t baseOff = colr + be32(d + colr + 4), layerOff = colr + be32(d + colr + 8);
    const std::uint16_t numEntries = be16(d + cpal + 2), numPalettes = be16(d + cpal + 4), numRecords = be16(d + cpal + 6);
    const std::uint32_t recOff = cpal + be32(d + cpal + 8);
    if (numPalettes == 0 || baseOff + std::size_t(numBase) * 6 > n || layerOff + std::size_t(numLayers) * 4 > n ||
        recOff + std::size_t(numRecords) * 4 > n)
        return;
    const std::uint16_t first = be16(d + cpal + 12); // palette 0's first colour record
    layers.reserve(numLayers);
    for (std::uint32_t i = 0; i < numLayers; ++i) {
        const std::uint8_t* l = d + layerOff + i * 4;
        ColorLayer cl;
        cl.glyph = be16(l);
        const std::uint16_t pi = be16(l + 2);
        if (pi == 0xFFFFu || pi >= numEntries || std::uint32_t(first) + pi >= numRecords) cl.foreground = true;
        else {
            const std::uint8_t* c = d + recOff + (std::uint32_t(first) + pi) * 4; // B G R A
            cl.b = c[0];
            cl.g = c[1];
            cl.r = c[2];
            cl.a = c[3];
        }
        layers.push_back(cl);
    }
    colorBase.reserve(numBase);
    for (std::uint32_t i = 0; i < numBase; ++i) {
        const std::uint8_t* b = d + baseOff + i * 6;
        const std::uint32_t firstLayer = be16(b + 2), count = be16(b + 4);
        if (firstLayer + count <= numLayers && count > 0) colorBase.push_back({be16(b), firstLayer, count});
    }
    std::sort(colorBase.begin(), colorBase.end());
}
} // namespace

std::expected<Font, TextError> Font::fromBytes(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12) return std::unexpected(TextError::InvalidFont);
    auto* st = new State;
    st->bytes.assign(bytes.begin(), bytes.end());
    const int offset = stbtt_GetFontOffsetForIndex(st->bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&st->info, st->bytes.data(), offset)) {
        delete st;
        return std::unexpected(TextError::InvalidFont);
    }
    st->unitsPerPixelScale1 = stbtt_ScaleForPixelHeight(&st->info, 1.f);
    st->emPerPixelSize = st->unitsPerPixelScale1 / stbtt_ScaleForMappingEmToPixels(&st->info, 1.f);
    loadColr(st->bytes, st->layers, st->colorBase, std::uint32_t(offset));
#if FUSE_TEXT_FULL
    st->full = detail::fullCreate(st->bytes);
    if (!st->full) {
        delete st;
        return std::unexpected(TextError::InvalidFont);
    }
#endif
    Font f;
    f.m_state = st;
    f.m_id = g_fontIds.fetch_add(1);
    return f;
}

std::expected<Font, TextError> Font::fromFile(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return std::unexpected(TextError::InvalidFont);
    std::vector<std::uint8_t> data;
    std::uint8_t buf[16384];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof buf, fp);
        data.insert(data.end(), buf, buf + n);
        if (n < sizeof buf) break;
    }
    std::fclose(fp);
    return fromBytes(data);
}

std::expected<Font, TextError> Font::bundled() {
#if defined(FUSE_TEXT_BUNDLED_FONT)
    return fromFile(FUSE_TEXT_BUNDLED_FONT);
#else
    return std::unexpected(TextError::InvalidFont);
#endif
}

std::vector<Font> Font::bundledFallbacks() {
    std::vector<Font> out;
#if defined(FUSE_TEXT_FONT_DIR)
    // Emoji before Symbols 2 so emoji-presentation code points (U+2764 ...) come out in colour.
    static const char* const kFiles[] = {"noto/NotoSansHebrew-Regular.ttf", "noto/NotoSansArabic-Regular.ttf",
                                         "noto/NotoSansDevanagari-Regular.ttf", "twemoji/TwemojiMozilla.ttf",
                                         "noto/NotoSansSymbols2-Regular.ttf"};
    for (const char* f : kFiles)
        if (auto font = fromFile(std::string(FUSE_TEXT_FONT_DIR) + "/" + f)) out.push_back(std::move(*font));
#endif
    return out;
}

void Font::setFallbacks(std::vector<const Font*> chain) {
    if (!m_state) return;
    std::erase_if(chain, [&](const Font* f) { return !f || !f->valid() || f == this; });
    m_state->fallbacks = std::move(chain);
}
std::size_t Font::faceCount() const { return m_state ? 1 + m_state->fallbacks.size() : 1; }
const Font& Font::face(std::uint16_t i) const {
    return (i == 0 || !m_state || i > m_state->fallbacks.size()) ? *this : *m_state->fallbacks[i - 1];
}
std::uint16_t Font::faceFor(char32_t cp) const {
    if (!m_state) return 0;
    if (glyphIndex(cp)) return 0;
    for (std::size_t k = 0; k < m_state->fallbacks.size(); ++k)
        if (m_state->fallbacks[k]->glyphIndex(cp)) return std::uint16_t(k + 1);
    return 0;
}
float Font::facePixelSize(std::uint16_t i, float px) const {
    const Font& f = face(i);
    if (&f == this) return px;
    return px * m_state->emPerPixelSize / f.m_state->emPerPixelSize;
}
bool Font::hasColorGlyphs() const { return m_state && !m_state->colorBase.empty(); }
std::span<const ColorLayer> Font::colorLayers(std::uint32_t glyph) const {
    if (!m_state || m_state->colorBase.empty()) return {};
    const auto& b = m_state->colorBase;
    auto it = std::lower_bound(b.begin(), b.end(), glyph, [](const auto& e, std::uint32_t g) { return e[0] < g; });
    if (it == b.end() || (*it)[0] != glyph) return {};
    return std::span<const ColorLayer>(m_state->layers.data() + (*it)[1], (*it)[2]);
}

Font::Font(Font&& o) noexcept : m_state(std::exchange(o.m_state, nullptr)), m_id(o.m_id) {}
Font& Font::operator=(Font&& o) noexcept {
    if (this != &o) {
        delete m_state;
        m_state = std::exchange(o.m_state, nullptr);
        m_id = o.m_id;
    }
    return *this;
}
Font::~Font() { delete m_state; }

FontMetrics Font::metrics(float px) const {
    int a = 0, d = 0, g = 0;
    stbtt_GetFontVMetrics(&m_state->info, &a, &d, &g);
    const float s = m_state->unitsPerPixelScale1 * px;
    return {float(a) * s, float(d) * s, float(g) * s};
}
std::uint32_t Font::glyphIndex(char32_t cp) const {
    return std::uint32_t(stbtt_FindGlyphIndex(&m_state->info, int(cp)));
}
float Font::advance(std::uint32_t glyph, float px) const {
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&m_state->info, int(glyph), &adv, &lsb);
    return float(adv) * m_state->unitsPerPixelScale1 * px;
}
float Font::kerning(std::uint32_t l, std::uint32_t r, float px) const {
    return float(stbtt_GetGlyphKernAdvance(&m_state->info, int(l), int(r))) * m_state->unitsPerPixelScale1 * px;
}
void Font::glyphBox(std::uint32_t glyph, float px, int& x0, int& y0, int& x1, int& y1) const {
#if FUSE_TEXT_FULL
    return detail::fullGlyphBox(m_state->full, glyph, m_state->emPerPixelSize * px, x0, y0, x1, y1);
#endif
    const float s = m_state->unitsPerPixelScale1 * px;
    stbtt_GetGlyphBitmapBox(&m_state->info, int(glyph), s, s, &x0, &y0, &x1, &y1);
}
void Font::rasterise(std::uint32_t glyph, float px, std::uint8_t* dst, int w, int h, int stride) const {
#if FUSE_TEXT_FULL
    return detail::fullRasterise(m_state->full, glyph, m_state->emPerPixelSize * px, dst, w, h, stride);
#endif
    const float s = m_state->unitsPerPixelScale1 * px;
    stbtt_MakeGlyphBitmap(&m_state->info, dst, w, h, stride, s, s, int(glyph));
}

// ---- Shaping -------------------------------------------------------------------------------------------------------
float MinimalShaper::shape(const Font& font, float px, std::string_view s, std::vector<ShapedGlyph>& out) const {
    out.clear();
    float pen = 0.f;
    std::uint32_t prev = 0;
    bool hasPrev = false;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t at = i;
        const char32_t cp = decodeUtf8(s, i);
        // Default-ignorable format characters (ZWSP..RLM, word joiner.., variation selectors, tags) have no ink and
        // no advance; without GSUB the sequences they steer fall back to their visible parts.
        if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
            cp == 0xFEFF || (cp >= 0xE0000 && cp <= 0xE0FFF)) {
            out.push_back({kInvisibleGlyph, std::uint32_t(at), pen, 0.f});
            continue;
        }
        const std::uint32_t g = font.glyphIndex(cp);
        if (hasPrev) pen += font.kerning(prev, g, px);
        const float adv = font.advance(g, px);
        out.push_back({g, std::uint32_t(at), pen, adv});
        pen += adv;
        prev = g;
        hasPrev = true;
    }
    return pen;
}

float Font::unitScale(float px) const { return m_state->unitsPerPixelScale1 * px; }

#if FUSE_TEXT_FULL
float HarfBuzzShaper::shape(const Font& font, float px, std::string_view s, std::vector<ShapedGlyph>& out) const {
    return detail::fullShape(font.m_state->full, font.unitScale(px), s, out);
}
const ITextShaper* fullShaper() {
    static const HarfBuzzShaper shaper;
    return &shaper;
}
#else
float HarfBuzzShaper::shape(const Font& font, float px, std::string_view s, std::vector<ShapedGlyph>& out) const {
    return MinimalShaper{}.shape(font, px, s, out); // FULL not compiled in
}
const ITextShaper* fullShaper() { return nullptr; }
#endif

const ITextShaper& defaultShaper() {
#if FUSE_TEXT_FULL
    return *fullShaper();
#else
    static const MinimalShaper shaper;
    return shaper;
#endif
}

// ---- Font fallback ---------------------------------------------------------------------------------------------
namespace {
// Code points that belong to the cluster before them keep its face (marks, ZWJ, emoji modifiers, VS16, tags, and
// the pictographic after a ZWJ), so a grapheme is never split across faces.
bool inheritsFace(char32_t cp, char32_t prev) {
    const GraphemeBreak p = graphemeBreakProperty(cp);
    return p == GraphemeBreak::Extend || p == GraphemeBreak::ZWJ || p == GraphemeBreak::SpacingMark ||
           (prev == 0x200D && p == GraphemeBreak::ExtPict);
}
bool mayNeedFallback(const Font& font, std::string_view s) {
    if (font.faceCount() < 2) return false;
    for (const char c : s)
        if (static_cast<unsigned char>(c) >= 0x80u) return true; // ASCII is always the primary face's
    return false;
}
/// Maximal same-face runs in logical order: fn(begin, end, face).
template <class Fn> void forEachFaceRun(const Font& font, std::string_view s, Fn&& fn) {
    std::size_t runBegin = 0;
    std::uint16_t runFace = 0;
    char32_t prev = 0;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t at = i;
        const char32_t cp = decodeUtf8(s, i);
        std::uint16_t f = 0;
        if (at > 0 && inheritsFace(cp, prev)) f = runFace;
        else if (cp >= 0x20 && cp != 0x7F) f = font.faceFor(cp);
        if (at == 0) runFace = f;
        else if (f != runFace) {
            fn(runBegin, at, runFace);
            runBegin = at;
            runFace = f;
        }
        prev = cp;
    }
    if (!s.empty()) fn(runBegin, s.size(), runFace);
}
float shapeWithFallback(const Font& font, const ITextShaper& shaper, float px, std::string_view s,
                        std::vector<ShapedGlyph>& out) {
    if (!mayNeedFallback(font, s)) return shaper.shape(font, px, s, out);
    out.clear();
    thread_local std::vector<ShapedGlyph> run;
    float pen = 0.f;
    forEachFaceRun(font, s, [&](std::size_t b, std::size_t e, std::uint16_t f) {
        const float w = shaper.shape(font.face(f), font.facePixelSize(f, px), s.substr(b, e - b), run);
        for (ShapedGlyph g : run) {
            g.cluster += std::uint32_t(b);
            g.x += pen;
            g.face = f;
            out.push_back(g);
        }
        pen += w;
    });
    return pen;
}
#if FUSE_TEXT_FULL
// FULL layout shapes with the primary face only (bidi + line breaking + run cache); fallback runs are then
// re-shaped with their face per line and the rest of the line shifts by the width change.
void applyFallbackPerLine(const Font& font, const ITextShaper& shaper, float px, std::string_view s,
                          std::vector<ShapedGlyph>& g, std::vector<TextLine>& lines) {
    if (!mayNeedFallback(font, s)) return;
    struct Span {
        std::uint32_t b, e;
        std::uint16_t face;
    };
    thread_local std::vector<Span> spans;
    spans.clear();
    forEachFaceRun(font, s, [&](std::size_t b, std::size_t e, std::uint16_t f) {
        if (f) spans.push_back({std::uint32_t(b), std::uint32_t(e), f});
    });
    if (spans.empty()) return;
    auto spanOf = [&](const ShapedGlyph& x) -> int {
        if (x.glyph == kInvisibleGlyph) return -1;
        for (std::size_t k = 0; k < spans.size(); ++k)
            if (x.cluster >= spans[k].b && x.cluster < spans[k].e) return int(k);
        return -1;
    };
    thread_local std::vector<ShapedGlyph> outG, run;
    outG.clear();
    for (TextLine& ln : lines) {
        const std::uint32_t first = std::uint32_t(outG.size());
        const std::uint32_t end = ln.firstGlyph + ln.glyphCount;
        float shift = 0.f;
        for (std::uint32_t k = ln.firstGlyph; k < end;) {
            const int sp = spanOf(g[k]);
            if (sp < 0) {
                ShapedGlyph x = g[k++];
                x.x += shift;
                outG.push_back(x);
                continue;
            }
            std::uint32_t j = k, lo = g[k].cluster;
            float oldAdv = 0.f;
            for (; j < end && spanOf(g[j]) == sp; ++j) {
                oldAdv += g[j].advance;
                lo = std::min(lo, g[j].cluster);
            }
            const Span& span = spans[std::size_t(sp)];
            std::uint32_t hi = span.e;
            if (ln.byteEnd > lo && ln.byteEnd < hi) hi = ln.byteEnd;
            const float x0 = g[k].x + shift;
            const float w = shaper.shape(font.face(span.face), font.facePixelSize(span.face, px), s.substr(lo, hi - lo), run);
            for (ShapedGlyph r : run) {
                r.cluster += lo;
                r.x += x0;
                r.face = span.face;
                outG.push_back(r);
            }
            shift += w - oldAdv;
            k = j;
        }
        ln.firstGlyph = first;
        ln.glyphCount = std::uint32_t(outG.size()) - first;
        ln.width += shift;
    }
    g.swap(outG);
}
#endif
} // namespace

float shapeText(const Font& font, std::string_view s, float px, std::vector<ShapedGlyph>& out) {
    return shapeWithFallback(font, defaultShaper(), px, s, out);
}

float measureText(const Font& font, std::string_view s, float px, std::vector<ShapedGlyph>& scratch) {
    return shapeText(font, s, px, scratch);
}

ShapeCacheStats shapeCacheStats(const Font& font) {
    ShapeCacheStats st;
#if FUSE_TEXT_FULL
    if (font.m_state) detail::fullCacheStats(font.m_state->full, st.hits, st.misses);
#else
    (void)font;
#endif
    return st;
}

void layoutText(const Font& font, const ITextShaper& shaper, std::string_view s, const TextLayoutDesc& desc,
                TextLayout& out) {
    out.lines.clear();
    out.metrics = font.metrics(desc.pixelSize);
    const float lh = std::ceil(out.metrics.lineHeight());
    const bool wrap = desc.maxWidth > 0.f && !desc.elide;
    auto& g = out.glyphs;
#if FUSE_TEXT_FULL
    const bool full = &shaper == fullShaper() && font.m_state && font.m_state->full;
#else
    const bool full = false;
#endif
    if (full) {
#if FUSE_TEXT_FULL
        detail::fullLayout(font.m_state->full, font.unitScale(desc.pixelSize), s, wrap ? desc.maxWidth : 0.f, lh,
                           out.metrics.ascent, g, out.lines);
        applyFallbackPerLine(font, shaper, desc.pixelSize, s, g, out.lines);
#endif
    } else {
    shapeWithFallback(font, shaper, desc.pixelSize, s, out.glyphs);
    const std::uint32_t n = std::uint32_t(g.size());

    std::uint32_t lineStart = 0;
    auto emit = [&](std::uint32_t begin, std::uint32_t end, std::uint32_t nextStart) {
        TextLine ln;
        ln.firstGlyph = begin;
        ln.glyphCount = end - begin;
        ln.byteBegin = begin < n ? g[begin].cluster : std::uint32_t(s.size());
        ln.byteEnd = end < n ? g[end].cluster : std::uint32_t(s.size());
        const float x0 = begin < n ? g[begin].x : 0.f;
        float w = 0.f;
        // Trailing spaces do not count toward the width.
        std::uint32_t last = end;
        while (last > begin && (s[g[last - 1].cluster] == ' ' || s[g[last - 1].cluster] == '\n')) --last;
        if (last > begin) w = g[last - 1].x + g[last - 1].advance - x0;
        for (std::uint32_t k = begin; k < end; ++k) g[k].x -= x0;
        ln.width = w;
        ln.baseline = float(out.lines.size()) * lh + std::ceil(out.metrics.ascent);
        out.lines.push_back(ln);
        lineStart = nextStart;
    };

    std::uint32_t lastBreak = 0; // glyph index after a space (break opportunity), 0 = none
    for (std::uint32_t i = 0; i < n; ++i) {
        const char c = s[g[i].cluster];
        if (c == '\n') {
            emit(lineStart, i + 1, i + 1);
            lastBreak = 0;
            continue;
        }
        if (wrap) {
            const float right = g[i].x + g[i].advance - g[lineStart].x;
            if (right > desc.maxWidth && c != ' ' && i > lineStart) {
                const std::uint32_t at = (lastBreak > lineStart) ? lastBreak : i;
                emit(lineStart, at, at);
                lastBreak = 0;
                // re-evaluate glyphs from `at` on the new line
                i = at - 1;
                continue;
            }
        }
        if (c == ' ') lastBreak = i + 1;
    }
    if (lineStart < n || out.lines.empty() || (n > 0 && s[g[n - 1].cluster] == '\n')) emit(lineStart, n, n);
    for (auto& gl : g) if (s[gl.cluster] == '\n' || s[gl.cluster] == '\r') gl.glyph = kInvisibleGlyph;
    }
    const std::uint32_t n = std::uint32_t(g.size());
    bool anyRtl = false;
    for (const auto& ln : out.lines) anyRtl |= ln.rtl;

    if (desc.elide && !anyRtl && desc.maxWidth > 0.f && !out.lines.empty() && out.lines[0].width > desc.maxWidth) {
        // Keep glyphs while they fit with an ellipsis, then append U+2026 (or "...").
        const std::uint32_t ell = font.glyphIndex(0x2026);
        const std::uint32_t dot = font.glyphIndex('.');
        const float ellW = ell ? font.advance(ell, desc.pixelSize) : 3.f * font.advance(dot, desc.pixelSize);
        std::uint32_t keep = 0;
        while (keep < n && g[keep].x + g[keep].advance + ellW <= desc.maxWidth) ++keep;
        float pen = keep ? g[keep - 1].x + g[keep - 1].advance : 0.f;
        g.resize(keep);
        const std::uint32_t at = keep < n ? std::uint32_t(s.size()) : std::uint32_t(s.size());
        if (ell) g.push_back({ell, at, pen, ellW});
        else for (int k = 0; k < 3; ++k) {
            const float a = font.advance(dot, desc.pixelSize);
            g.push_back({dot, at, pen, a});
            pen += a;
        }
        out.lines.resize(1);
        out.lines[0].glyphCount = std::uint32_t(g.size());
        out.lines[0].width = g.empty() ? 0.f : g.back().x + g.back().advance;
    }

    out.width = 0.f;
    for (const auto& ln : out.lines) out.width = std::max(out.width, ln.width);
    const float alignW = desc.maxWidth > 0.f ? desc.maxWidth : out.width;
    if (desc.align != TextAlign::Start || anyRtl) {
        for (const auto& ln : out.lines) {
            // Start / End follow the paragraph direction: Start is the right edge of an RTL line.
            const bool toEnd = (desc.align == TextAlign::End) != ln.rtl;
            if (desc.align != TextAlign::Center && !toEnd) continue;
            const float off = desc.align == TextAlign::Center ? std::floor((alignW - ln.width) * 0.5f) : alignW - ln.width;
            for (std::uint32_t k = ln.firstGlyph; k < ln.firstGlyph + ln.glyphCount; ++k) g[k].x += off;
        }
    }
    out.height = float(out.lines.size()) * lh;
}

// ---- Glyph atlas ---------------------------------------------------------------------------------------------------
GlyphAtlas::GlyphAtlas(int size) : m_size(size), m_pixels(std::size_t(size) * std::size_t(size), 0), m_slots(4096) {}

void GlyphAtlas::clear() {
    std::fill(m_pixels.begin(), m_pixels.end(), std::uint8_t(0));
    for (auto& s : m_slots) s = Slot{};
    m_used = 0;
    m_shelfX = 1;
    m_shelfY = 1;
    m_shelfH = 0;
    ++m_generation;
}

AtlasGlyph GlyphAtlas::get(const Font& font, std::uint32_t glyph, float px) {
    const std::uint32_t q = std::uint32_t(std::lround(px * 4.f));
    const std::uint64_t key = (std::uint64_t(font.id()) << 44) ^ (std::uint64_t(q) << 24) ^ glyph ^ (1ull << 63);
    const std::size_t mask = m_slots.size() - 1;
    std::size_t h = std::size_t((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
    for (;;) {
        Slot& s = m_slots[h];
        if (s.key == key) return s.g;
        if (s.key == 0) break;
        h = (h + 1) & mask;
    }
    ++m_misses;
    if (m_used * 4 >= m_slots.size() * 3) {
        clear();
        return get(font, glyph, px);
    }
    const float pxq = float(q) / 4.f;
    int x0, y0, x1, y1;
    font.glyphBox(glyph, pxq, x0, y0, x1, y1);
    const int w = x1 - x0, hh = y1 - y0;
    AtlasGlyph ag;
    ag.x0 = std::int16_t(x0);
    ag.y0 = std::int16_t(y0);
    if (w > 0 && hh > 0) {
        if (m_shelfX + w + 1 > m_size) {
            m_shelfX = 1;
            m_shelfY += m_shelfH + 1;
            m_shelfH = 0;
        }
        if (m_shelfY + hh + 1 > m_size) {
            clear();
            return get(font, glyph, px);
        }
        font.rasterise(glyph, pxq, m_pixels.data() + std::size_t(m_shelfY) * std::size_t(m_size) + std::size_t(m_shelfX),
                       w, hh, m_size);
        ag.u = std::uint16_t(m_shelfX);
        ag.v = std::uint16_t(m_shelfY);
        ag.w = std::uint16_t(w);
        ag.h = std::uint16_t(hh);
        m_shelfX += w + 1;
        m_shelfH = std::max(m_shelfH, hh);
    }
    ag.valid = true;
    m_slots[h] = {key, ag};
    ++m_used;
    return ag;
}

// ---- Editing -------------------------------------------------------------------------------------------------------
namespace {
bool isWordByte(char c) { return (static_cast<unsigned char>(c) >= 0x80u) || std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
}

void TextEditBuffer::setText(std::string_view s) {
    m_text.assign(s.substr(0, m_maxLength));
    m_cursor = m_anchor = m_text.size();
    m_undo.clear();
    m_redo.clear();
}
std::string_view TextEditBuffer::selection() const {
    const auto a = std::min(m_cursor, m_anchor), b = std::max(m_cursor, m_anchor);
    return std::string_view(m_text).substr(a, b - a);
}
void TextEditBuffer::record(std::string before, std::size_t cursorBefore) {
    m_undo.push_back({std::move(before), m_text, cursorBefore, m_cursor});
    m_redo.clear();
}
void TextEditBuffer::replaceSelection(std::string_view s) {
    const auto a = std::min(m_cursor, m_anchor), b = std::max(m_cursor, m_anchor);
    std::string before = m_text;
    const std::size_t cb = m_cursor;
    std::size_t room = m_maxLength - (m_text.size() - (b - a));
    if (s.size() > room) {
        // never split a code point
        std::size_t k = room;
        while (k > 0 && k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0u) == 0x80u) --k;
        s = s.substr(0, k);
    }
    m_text.replace(a, b - a, s);
    m_cursor = m_anchor = a + s.size();
    if (before != m_text) record(std::move(before), cb);
}
void TextEditBuffer::insert(std::string_view s) { replaceSelection(s); }
void TextEditBuffer::backspace() {
    if (!hasSelection()) m_anchor = prevBoundary(m_text, m_cursor);
    replaceSelection({});
}
void TextEditBuffer::del() {
    if (!hasSelection()) m_anchor = nextBoundary(m_text, m_cursor);
    replaceSelection({});
}
void TextEditBuffer::moveLeft(bool sel) {
    if (!sel && hasSelection()) m_cursor = std::min(m_cursor, m_anchor);
    else m_cursor = prevBoundary(m_text, m_cursor);
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::moveRight(bool sel) {
    if (!sel && hasSelection()) m_cursor = std::max(m_cursor, m_anchor);
    else m_cursor = nextBoundary(m_text, m_cursor);
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::wordLeft(bool sel) {
    std::size_t i = m_cursor;
    while (i > 0 && !isWordByte(m_text[i - 1])) --i;
    while (i > 0 && isWordByte(m_text[i - 1])) --i;
    m_cursor = i;
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::wordRight(bool sel) {
    std::size_t i = m_cursor;
    const std::size_t n = m_text.size();
    while (i < n && isWordByte(m_text[i])) ++i;
    while (i < n && !isWordByte(m_text[i])) ++i;
    m_cursor = i;
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::home(bool sel) {
    std::size_t i = m_cursor;
    while (i > 0 && m_text[i - 1] != '\n') --i;
    m_cursor = i;
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::end(bool sel) {
    std::size_t i = m_cursor;
    while (i < m_text.size() && m_text[i] != '\n') ++i;
    m_cursor = i;
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::selectAll() {
    m_anchor = 0;
    m_cursor = m_text.size();
}
bool TextEditBuffer::undo() {
    if (m_undo.empty()) return false;
    Edit e = std::move(m_undo.back());
    m_undo.pop_back();
    m_text = e.before;
    m_cursor = m_anchor = e.cursorBefore;
    m_redo.push_back(std::move(e));
    return true;
}
bool TextEditBuffer::redo() {
    if (m_redo.empty()) return false;
    Edit e = std::move(m_redo.back());
    m_redo.pop_back();
    m_text = e.after;
    m_cursor = m_anchor = e.cursorAfter;
    m_undo.push_back(std::move(e));
    return true;
}

} // namespace fuse::text
