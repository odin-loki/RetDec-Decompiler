// fuse_ui_text_basics (UI3 MINIMAL): bundled font, UTF-8, measure / wrap / ellipsis, atlas caching, edit buffer.
#include "test_util.hpp"

using namespace fuse::text;
using fuse::ui::test::font;

int main() {
    #if FUSE_UI_TEST_TEXT_FULL
    UI_CHECK(textProfile() == TextProfile::Full);
#else
    UI_CHECK(textProfile() == TextProfile::Minimal);
#endif
    std::size_t i = 0;
    UI_CHECK(decodeUtf8("\xE2\x80\x94", i) == U'\x2014' && i == 3);
    std::string s;
    appendUtf8(s, U'\x1F600');
    UI_CHECK(s.size() == 4);
    UI_CHECK(prevBoundary("a\xC3\xA9", 3) == 1);
    UI_CHECK(nextBoundary("a\xC3\xA9", 1) == 3);

    const Font& f = font();
    std::vector<ShapedGlyph> scratch;
    const float wA = measureText(f, "Hello", 16.f, scratch), wB = measureText(f, "Hello world", 16.f, scratch);
    UI_CHECK(wA > 20.f && wA < 60.f);
    UI_CHECK(wB > wA);
    UI_CHECK(measureText(f, "Hello", 32.f, scratch) > 1.9f * wA);
    UI_CHECK(f.glyphIndex(U'A') != 0);

    GlyphAtlas atlas(256);
    const AtlasGlyph g1 = atlas.get(f, f.glyphIndex(U'A'), 16.f);
    const AtlasGlyph g2 = atlas.get(f, f.glyphIndex(U'A'), 16.f);
    UI_CHECK(g1.w > 0 && g1.w == g2.w && g1.u == g2.u && g1.v == g2.v);

    TextEditBuffer b;
    b.setText("hello world");
    b.end(false);
    b.wordLeft(true);
    UI_CHECK(b.selection() == "world");
    b.insert("there");
    UI_CHECK(b.text() == "hello there");
    UI_CHECK(b.undo());
    UI_CHECK(b.text() == "hello world");
    UI_CHECK(b.redo());
    b.selectAll();
    b.backspace();
    UI_CHECK(b.text().empty());
    return fuse::ui::test::finish("fuse_ui_text_basics");
}
