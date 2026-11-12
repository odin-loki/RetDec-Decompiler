// fuse_text_edit_ops (UI3): UAX #29 extended grapheme clusters drive caret moves, Backspace and Delete in
// TextEditBuffer, so combining marks, CR LF, Hangul jamo, flags, skin tones, ZWJ emoji and keycaps never split.
#include "test_util.hpp"

#include <vector>

using namespace fuse::text;

namespace {
std::string u8(std::u32string_view cps) {
    std::string s;
    for (char32_t c : cps) appendUtf8(s, c);
    return s;
}
// every boundary walking forward, then backward: the two walks must agree
std::vector<std::size_t> forward(std::string_view s) {
    std::vector<std::size_t> b{0};
    for (std::size_t i = 0; i < s.size();) b.push_back(i = nextBoundary(s, i));
    return b;
}
std::vector<std::size_t> backward(std::string_view s) {
    std::vector<std::size_t> b{s.size()};
    for (std::size_t i = s.size(); i > 0;) b.insert(b.begin(), i = prevBoundary(s, i));
    return b;
}
struct Case { std::u32string text; std::size_t clusters; const char* what; };
} // namespace

int main() {
    const Case cases[] = {
        {U"abc", 3, "ASCII"},
        {U"e\u0301", 1, "e + combining acute (GB9)"},
        {U"e\u0301\u0323x", 2, "two marks stay on the base"},
        {U"\r\n", 1, "CR LF (GB3)"},
        {U"\n\r", 2, "LF CR"},
        {U"a\r\nb", 3, "CR LF between letters"},
        {U"a\u0001\u0301", 3, "controls break (GB4/GB5)"},
        {U"\u1100\u1161\u11A8", 1, "Hangul L V T (GB6/GB7)"},
        {U"\uAC00\u11A8\uAC01", 2, "Hangul LV T | LVT"},
        {U"\U0001F1E6\U0001F1FA\U0001F1F3\U0001F1FF", 2, "two flags (GB12/13)"},
        {U"\U0001F1E6\U0001F1FA\U0001F1F3", 2, "flag + lone RI"},
        {U"\U0001F44D\U0001F3FD", 1, "thumbs up + skin tone"},
        {U"\U0001F468\u200D\U0001F469\u200D\U0001F467", 1, "ZWJ family (GB11)"},
        {U"a\u200Db", 2, "ZWJ after a letter does not join (GB11 needs ExtPict)"},
        {U"\u2764\uFE0F", 1, "heart + VS16"},
        {U"#\uFE0F\u20E3", 1, "keycap"},
        {U"\u0600a", 1, "Prepend (GB9b)"},
        {U"\u0915\u093F", 1, "Devanagari spacing mark (GB9a)"},
        {U"\u0915\u094D\u0937\u093F", 1, "Devanagari conjunct ksha + i (GB9c)"},
        {U"\u0928\u092E\u0938\u094D\u0924\u0947", 3, "namaste: na | ma | ste"},
        {U"\u0915\u094D\u200D\u0937", 1, "virama + ZWJ still links (GB9c)"},
        {U"\u0915\u093F\u0937", 2, "no linker: no conjunct"},
        {U"a\u094D\u0915", 2, "linker after a non-consonant does not link"},
        {U"\u0995\u09CD\u09B7", 1, "Bengali conjunct"},
        {U"\u0E01\u0E33", 1, "Thai SARA AM (SpacingMark)"},
        {U"\U0001F3F4\U000E0067\U000E0062\U000E0073\U000E0063\U000E0074\U000E007F", 1, "tag sequence flag"},
    };
    for (const Case& c : cases) {
        const std::string s = u8(c.text);
        const std::size_t n = graphemeCount(s);
        const auto f = forward(s), b = backward(s);
        const bool ok = n == c.clusters && f == b && f.size() == c.clusters + 1;
        if (!ok) std::printf("  %s: %zu clusters (want %zu), forward %zu / backward %zu stops\n", c.what, n, c.clusters, f.size(), b.size());
        UI_CHECK(ok);
    }
    UI_CHECK(graphemeBreakProperty(U'\u0301') == GraphemeBreak::Extend && graphemeBreakProperty(U'\U0001F600') == GraphemeBreak::ExtPict);
    UI_CHECK(prevCodepoint("a\xC3\xA9", 3) == 1 && nextCodepoint("a\xC3\xA9", 1) == 3);

    // Editing: x <family> y <e + acute> z
    const std::string family = u8(U"\U0001F468\u200D\U0001F469\u200D\U0001F467");
    const std::string eAcute = u8(U"e\u0301");
    TextEditBuffer b;
    b.setText("x" + family + "y" + eAcute + "z");
    b.home(false);
    std::size_t stops = 0;
    while (b.cursor() < b.text().size()) { b.moveRight(false); ++stops; }
    UI_CHECK(stops == 5); // x, family, y, e-acute, z
    b.end(false);
    b.moveLeft(false);                 // before z
    b.moveLeft(true);                  // select e + acute as one
    UI_CHECK(b.selection() == eAcute);
    b.moveLeft(false);
    b.moveLeft(false);                 // before y
    b.backspace();                     // removes the whole family
    UI_CHECK(b.text() == "xy" + eAcute + "z");
    b.moveRight(false);                // after y
    b.del();                           // removes e + acute together
    UI_CHECK(b.text() == "xyz");
    UI_CHECK(b.undo() && b.text() == "xy" + eAcute + "z");
    UI_CHECK(b.undo() && b.text() == "x" + family + "y" + eAcute + "z");
    // CR LF is one caret step and one Backspace
    b.setText("a\r\nb");
    b.end(false);
    b.moveLeft(false);
    b.backspace();
    UI_CHECK(b.text() == "ab");
    std::printf("fuse_text_edit_ops: %zu cluster cases\n", sizeof(cases) / sizeof(cases[0]));
    return fuse::ui::test::finish("fuse_text_edit_ops");
}
