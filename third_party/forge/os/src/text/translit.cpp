// forge::os::text: transliteration to ASCII, mark removal and slugs.
#include "internal.hpp"
#include <algorithm>
#include <cctype>

namespace forge::os::text {
using namespace detail;
namespace {
struct tr { cp_t c; const char* a; };
// Sorted by code point. Lowercase forms only for alphabets; uppercase is derived (first letter capitalised).
constexpr tr table[] = {
    {0x00A0, " "}, {0x00A1, "!"}, {0x00A2, "c"}, {0x00A3, "GBP"}, {0x00A5, "JPY"}, {0x00A6, "|"}, {0x00A7, "S"}, {0x00A9, "(C)"}, {0x00AA, "a"},
    {0x00AB, "<<"}, {0x00AC, "!"}, {0x00AD, ""}, {0x00AE, "(R)"}, {0x00B0, "deg"}, {0x00B1, "+/-"}, {0x00B5, "u"}, {0x00B6, "P"}, {0x00B7, "."},
    {0x00BA, "o"}, {0x00BB, ">>"}, {0x00BF, "?"}, {0x00C6, "AE"}, {0x00D0, "D"}, {0x00D7, "x"}, {0x00D8, "O"}, {0x00DE, "TH"}, {0x00DF, "ss"},
    {0x00E6, "ae"}, {0x00F0, "d"}, {0x00F7, "/"}, {0x00F8, "o"}, {0x00FE, "th"}, {0x0110, "D"}, {0x0111, "d"}, {0x0126, "H"}, {0x0127, "h"},
    {0x0131, "i"}, {0x0132, "IJ"}, {0x0133, "ij"}, {0x0138, "k"}, {0x013F, "L"}, {0x0140, "l"}, {0x0141, "L"}, {0x0142, "l"}, {0x0149, "'n"},
    {0x014A, "NG"}, {0x014B, "ng"}, {0x0152, "OE"}, {0x0153, "oe"}, {0x0166, "T"}, {0x0167, "t"}, {0x017F, "s"}, {0x0180, "b"}, {0x0181, "B"},
    {0x0186, "O"}, {0x0189, "D"}, {0x018A, "D"}, {0x018E, "E"}, {0x018F, "E"}, {0x0190, "E"}, {0x0191, "F"}, {0x0192, "f"}, {0x0193, "G"},
    {0x0197, "I"}, {0x0198, "K"}, {0x0199, "k"}, {0x019A, "l"}, {0x019D, "N"}, {0x019E, "n"}, {0x01A4, "P"}, {0x01A5, "p"}, {0x01AB, "t"},
    {0x01AC, "T"}, {0x01AD, "t"}, {0x01AE, "T"}, {0x01B2, "V"}, {0x01B3, "Y"}, {0x01B4, "y"}, {0x01B5, "Z"}, {0x01B6, "z"}, {0x0221, "d"},
    {0x0234, "l"}, {0x0235, "n"}, {0x0236, "t"}, {0x0237, "j"}, {0x0238, "db"}, {0x0239, "qp"}, {0x023A, "A"}, {0x023B, "C"}, {0x023C, "c"},
    {0x023D, "L"}, {0x023E, "T"}, {0x0250, "a"}, {0x0253, "b"}, {0x0254, "o"}, {0x0256, "d"}, {0x0257, "d"}, {0x0259, "e"}, {0x025B, "e"},
    {0x0260, "g"}, {0x0261, "g"}, {0x0265, "h"}, {0x0266, "h"}, {0x0268, "i"}, {0x026B, "l"}, {0x026C, "l"}, {0x026D, "l"}, {0x0271, "m"},
    {0x0272, "n"}, {0x0273, "n"}, {0x0275, "o"}, {0x027C, "r"}, {0x027D, "r"}, {0x027E, "r"}, {0x0282, "s"}, {0x0288, "t"}, {0x0289, "u"},
    {0x028B, "v"}, {0x0290, "z"}, {0x0291, "z"}, {0x02B9, "'"}, {0x02BA, "\""}, {0x02BB, "'"}, {0x02BC, "'"}, {0x02C6, "^"}, {0x02C8, "'"},
    {0x02DC, "~"},
    // Greek
    {0x03B1, "a"}, {0x03B2, "v"}, {0x03B3, "g"}, {0x03B4, "d"}, {0x03B5, "e"}, {0x03B6, "z"}, {0x03B7, "i"}, {0x03B8, "th"}, {0x03B9, "i"},
    {0x03BA, "k"}, {0x03BB, "l"}, {0x03BC, "m"}, {0x03BD, "n"}, {0x03BE, "x"}, {0x03BF, "o"}, {0x03C0, "p"}, {0x03C1, "r"}, {0x03C2, "s"},
    {0x03C3, "s"}, {0x03C4, "t"}, {0x03C5, "y"}, {0x03C6, "f"}, {0x03C7, "ch"}, {0x03C8, "ps"}, {0x03C9, "o"},
    // Cyrillic
    {0x0430, "a"}, {0x0431, "b"}, {0x0432, "v"}, {0x0433, "g"}, {0x0434, "d"}, {0x0435, "e"}, {0x0436, "zh"}, {0x0437, "z"}, {0x0438, "i"},
    {0x0439, "y"}, {0x043A, "k"}, {0x043B, "l"}, {0x043C, "m"}, {0x043D, "n"}, {0x043E, "o"}, {0x043F, "p"}, {0x0440, "r"}, {0x0441, "s"},
    {0x0442, "t"}, {0x0443, "u"}, {0x0444, "f"}, {0x0445, "kh"}, {0x0446, "ts"}, {0x0447, "ch"}, {0x0448, "sh"}, {0x0449, "shch"}, {0x044A, ""},
    {0x044B, "y"}, {0x044C, ""}, {0x044D, "e"}, {0x044E, "yu"}, {0x044F, "ya"}, {0x0451, "e"}, {0x0452, "dj"}, {0x0453, "gj"}, {0x0454, "ye"},
    {0x0455, "dz"}, {0x0456, "i"}, {0x0457, "yi"}, {0x0458, "j"}, {0x0459, "lj"}, {0x045A, "nj"}, {0x045B, "c"}, {0x045C, "kj"}, {0x045E, "u"},
    {0x045F, "dz"}, {0x0491, "g"}, {0x0493, "gh"}, {0x049B, "q"}, {0x04A3, "ng"}, {0x04AF, "u"}, {0x04B1, "u"}, {0x04BB, "h"}, {0x04D9, "a"},
    {0x04E9, "o"},
    // Hebrew (consonants)
    {0x05D0, "'"}, {0x05D1, "b"}, {0x05D2, "g"}, {0x05D3, "d"}, {0x05D4, "h"}, {0x05D5, "v"}, {0x05D6, "z"}, {0x05D7, "kh"}, {0x05D8, "t"},
    {0x05D9, "y"}, {0x05DA, "kh"}, {0x05DB, "k"}, {0x05DC, "l"}, {0x05DD, "m"}, {0x05DE, "m"}, {0x05DF, "n"}, {0x05E0, "n"}, {0x05E1, "s"},
    {0x05E2, "'"}, {0x05E3, "f"}, {0x05E4, "p"}, {0x05E5, "ts"}, {0x05E6, "ts"}, {0x05E7, "k"}, {0x05E8, "r"}, {0x05E9, "sh"}, {0x05EA, "t"},
    // Arabic (basic letters)
    {0x0621, "'"}, {0x0622, "a"}, {0x0623, "a"}, {0x0624, "w"}, {0x0625, "i"}, {0x0626, "y"}, {0x0627, "a"}, {0x0628, "b"}, {0x0629, "h"},
    {0x062A, "t"}, {0x062B, "th"}, {0x062C, "j"}, {0x062D, "h"}, {0x062E, "kh"}, {0x062F, "d"}, {0x0630, "dh"}, {0x0631, "r"}, {0x0632, "z"},
    {0x0633, "s"}, {0x0634, "sh"}, {0x0635, "s"}, {0x0636, "d"}, {0x0637, "t"}, {0x0638, "z"}, {0x0639, "'"}, {0x063A, "gh"}, {0x0641, "f"},
    {0x0642, "q"}, {0x0643, "k"}, {0x0644, "l"}, {0x0645, "m"}, {0x0646, "n"}, {0x0647, "h"}, {0x0648, "w"}, {0x0649, "a"}, {0x064A, "y"},
    {0x067E, "p"}, {0x0686, "ch"}, {0x0698, "zh"}, {0x06A9, "k"}, {0x06AF, "g"}, {0x06CC, "y"},
    // Punctuation and symbols
    {0x2002, " "}, {0x2003, " "}, {0x2004, " "}, {0x2005, " "}, {0x2006, " "}, {0x2007, " "}, {0x2008, " "}, {0x2009, " "}, {0x200A, " "},
    {0x200B, ""}, {0x200C, ""}, {0x200D, ""}, {0x2010, "-"}, {0x2011, "-"}, {0x2012, "-"}, {0x2013, "-"}, {0x2014, "-"}, {0x2015, "-"},
    {0x2016, "||"}, {0x2018, "'"}, {0x2019, "'"}, {0x201A, ","}, {0x201B, "'"}, {0x201C, "\""}, {0x201D, "\""}, {0x201E, ",,"}, {0x201F, "\""},
    {0x2020, "+"}, {0x2022, "*"}, {0x2024, "."}, {0x2025, ".."}, {0x2026, "..."}, {0x202F, " "}, {0x2030, "%0"}, {0x2032, "'"}, {0x2033, "\""},
    {0x2039, "<"}, {0x203A, ">"}, {0x2044, "/"}, {0x205F, " "}, {0x2060, ""}, {0x20A9, "W"}, {0x20AA, "ILS"}, {0x20AB, "VND"}, {0x20AC, "EUR"},
    {0x20B9, "INR"}, {0x20BA, "TRY"}, {0x20BD, "RUB"}, {0x2116, "No"}, {0x2122, "TM"}, {0x2190, "<-"}, {0x2192, "->"}, {0x2194, "<->"},
    {0x21D0, "<="}, {0x21D2, "=>"}, {0x2212, "-"}, {0x2215, "/"}, {0x2217, "*"}, {0x2218, "o"}, {0x221E, "inf"}, {0x2248, "~"}, {0x2260, "!="},
    {0x2264, "<="}, {0x2265, ">="}, {0x2500, "-"}, {0x2502, "|"}, {0x250C, "+"}, {0x2510, "+"}, {0x2514, "+"}, {0x2518, "+"}, {0x251C, "+"},
    {0x2524, "+"}, {0x252C, "+"}, {0x2534, "+"}, {0x253C, "+"}, {0x2550, "="}, {0x2551, "|"}, {0x25CF, "*"}, {0x2605, "*"}, {0x2713, "v"},
    {0x2717, "x"}, {0x3000, " "}, {0x3001, ","}, {0x3002, "."}, {0x300C, "\""}, {0x300D, "\""}, {0xFEFF, ""}, {0xFF5E, "~"},
};
const char* find(cp_t c) {
    auto it = std::lower_bound(std::begin(table), std::end(table), c, [](const tr& t, cp_t x) { return t.c < x; });
    return (it != std::end(table) && it->c == c) ? it->a : nullptr;
}
}  // namespace

std::string remove_marks(std::string_view s) {
    auto v = normalize32(to32(s), norm_form::nfd);
    std::u32string o; for (auto c : v) if (general_category(c) != category::Mn) o += c;
    return encode_utf8(normalize32(o, norm_form::nfc));
}
std::string to_ascii(std::string_view s, char unknown) {
    auto v = normalize32(to32(s), norm_form::nfkd);
    std::string o; o.reserve(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        cp_t c = v[i];
        if (c < 0x80) { o += char(c); continue; }
        if (general_category(c) == category::Mn || general_category(c) == category::Me) continue;
        if (is_space(c)) { o += ' '; continue; }
        if (auto d = digit_value(c)) { o += char('0' + *d); continue; }
        cp_t lc = simple_lower(c); bool up = lc != c;
        const char* a = find(c); if (!a && up) a = find(lc);
        if (!a) { if (is_default_ignorable(c)) continue; if (unknown) o += unknown; continue; }
        std::string t(a);
        if (up && !t.empty()) {
            // All-caps when the next letter is uppercase too ("ЩИ" -> "SHCHI"), else capitalise ("Щи" -> "Shchi").
            bool next_up = i + 1 < v.size() && is_upper(v[i + 1]);
            if (next_up) for (auto& ch : t) ch = char(std::toupper((unsigned char)ch));
            else t[0] = char(std::toupper((unsigned char)t[0]));
        }
        o += t;
    }
    return o;
}
std::string slugify(std::string_view s, char sep, std::size_t max_len) {
    auto a = to_ascii(s, 0);
    std::string o; bool pend = false;
    for (char ch : a) {
        unsigned char c = (unsigned char)ch;
        if (std::isalnum(c)) { if (pend && !o.empty()) o += sep; pend = false; o += char(std::tolower(c)); }
        else if (ch == '\'') {}
        else pend = true;
    }
    if (max_len && o.size() > max_len) {
        o.resize(max_len);
        if (auto p = o.rfind(sep); p != std::string::npos && p > max_len / 2) o.resize(p);
        while (!o.empty() && o.back() == sep) o.pop_back();
    }
    return o;
}
}  // namespace forge::os::text
