// fuse_ui_text_full: the FULL text profile (HarfBuzz shaping + FreeType coverage). In MINIMAL builds it checks that
// fullShaper() is absent and HarfBuzzShaper falls back to the minimal shaper.
#include "test_util.hpp"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <vector>

using namespace fuse::text;
using fuse::ui::test::font;

int main() {
    const Font& f = font();
    std::vector<ShapedGlyph> a, b;
#if FUSE_UI_TEST_TEXT_FULL
    UI_CHECK(textProfile() == TextProfile::Full);
    UI_CHECK(fullShaper() != nullptr);
    // HarfBuzz and the cmap+kern shaper agree on simple Latin within a pixel per glyph.
    const float wh = fullShaper()->shape(f, 16.f, "Hello", a);
    const float wm = MinimalShaper{}.shape(f, 16.f, "Hello", b);
    UI_CHECK(a.size() == 5 && std::abs(wh - wm) < 5.f);
    // Clusters are byte offsets and non-decreasing for an LTR run; multibyte input keeps byte clusters.
    fullShaper()->shape(f, 16.f, "a\xC3\xA9z", a);
    UI_CHECK(a.size() == 3 && a[0].cluster == 0 && a[1].cluster == 1 && a[2].cluster == 3);
    // Combining acute after 'e': the mark gets zero advance (GPOS / mark positioning) or composes into U+00E9.
    fullShaper()->shape(f, 16.f, "e\xCC\x81", a);
    UI_CHECK(a.size() == 1 || (a.size() == 2 && a[1].advance == 0.f));
    // FreeType coverage: a visible glyph has a non-empty box and some ink.
    const std::uint32_t g = f.glyphIndex('A');
    int x0, y0, x1, y1;
    f.glyphBox(g, 24.f, x0, y0, x1, y1);
    UI_CHECK(x1 > x0 && y1 > y0 && y0 < 0);
    std::vector<std::uint8_t> px(std::size_t((x1 - x0) * (y1 - y0)), 0);
    f.rasterise(g, 24.f, px.data(), x1 - x0, y1 - y0, x1 - x0);
    int ink = 0;
    for (auto v : px) ink += v > 128;
    UI_CHECK(ink > 10);
    // Steady state: reshaping into a buffer with capacity does not allocate (capacity kept).
    const auto cap = a.capacity();
    fullShaper()->shape(f, 16.f, "e", a);
    UI_CHECK(a.capacity() == cap);

    // ---- bidi (UAX #9) + UAX #14 line breaking + run cache ----
    TextLayout L;
    TextLayoutDesc d;
    d.pixelSize = 16.f;
    auto clusters = [&](const TextLine& ln) {
        std::vector<std::uint32_t> c;
        for (std::uint32_t k = ln.firstGlyph; k < ln.firstGlyph + ln.glyphCount; ++k) c.push_back(L.glyphs[k].cluster);
        return c;
    };
    auto increasingX = [&](const TextLine& ln) {
        for (std::uint32_t k = ln.firstGlyph + 1; k < ln.firstGlyph + ln.glyphCount; ++k)
            if (L.glyphs[k].x < L.glyphs[k - 1].x) return false;
        return true;
    };
    // LTR paragraph with an embedded Hebrew word: "ab " + alef bet gimel + " cd". Visual: a b ' ' gimel bet alef ' ' c d.
    const char* mixed = "ab \xD7\x90\xD7\x91\xD7\x92 cd";
    layoutText(f, *fullShaper(), mixed, d, L);
    UI_CHECK(L.lines.size() == 1 && !L.lines[0].rtl);
    {
        const auto c = clusters(L.lines[0]);
        const std::vector<std::uint32_t> want{0, 1, 2, 7, 5, 3, 9, 10, 11};
        UI_CHECK(c == want);
        UI_CHECK(increasingX(L.lines[0]));
    }
    // RTL paragraph: hebrew word, space, hebrew word -> the second word is leftmost; Start alignment is the right edge.
    const char* rtl = "\xD7\x90\xD7\x91 \xD7\x92\xD7\x93";
    d.maxWidth = 200.f;
    layoutText(f, *fullShaper(), rtl, d, L);
    UI_CHECK(L.lines.size() == 1 && L.lines[0].rtl);
    {
        const auto c = clusters(L.lines[0]);
        const std::vector<std::uint32_t> want{7, 5, 4, 2, 0};
        UI_CHECK(c == want);
        const auto& last = L.glyphs[L.lines[0].firstGlyph + L.lines[0].glyphCount - 1];
        UI_CHECK(std::abs(last.x + last.advance - 200.f) < 1.f); // right-aligned
    }
    // Numbers inside RTL keep LTR order (weak types, W/N rules): alef " 12 " bet -> bet, 1, 2, alef visually.
    d.maxWidth = 0.f;
    layoutText(f, *fullShaper(), "\xD7\x90 12 \xD7\x91", d, L);
    {
        const auto c = clusters(L.lines[0]);
        const std::vector<std::uint32_t> want{6, 5, 3, 4, 2, 0};
        UI_CHECK(L.lines[0].rtl && c == want);
    }
    // UAX #14: a break after the hyphen of "well-known"; ideographs break between characters without spaces.
    d.maxWidth = std::max(measureText(f, "well-", 16.f, a), measureText(f, "known", 16.f, a)) + 2.f;
    layoutText(f, *fullShaper(), "well-known", d, L);
    UI_CHECK(L.lines.size() == 2 && L.lines[1].byteBegin == 5);
    d.maxWidth = 0.f;
    layoutText(f, *fullShaper(), "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", d, L); // 日本語
    const float one = L.lines[0].width / 3.f;
    d.maxWidth = one * 2.f + 0.5f;
    layoutText(f, *fullShaper(), "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", d, L);
    UI_CHECK(L.lines.size() == 2 && L.lines[1].byteBegin == 6);
    // Hard line breaks split lines but are never drawn (no tofu glyph at line ends).
    d.maxWidth = 0.f;
    layoutText(f, *fullShaper(), "ab\ncd\n", d, L);
    UI_CHECK(L.lines.size() == 3);
    for (const auto& g : L.glyphs) UI_CHECK((g.cluster == 2 || g.cluster == 5) == (g.glyph == kInvisibleGlyph));
    // Words wrap at spaces; trailing spaces do not count.
    d.maxWidth = measureText(f, "hello world", 16.f, a) + 1.f;
    layoutText(f, *fullShaper(), "hello world again", d, L);
    UI_CHECK(L.lines.size() == 2 && L.lines[1].byteBegin == 12);
    // Run cache: relaying out the same text hits, never misses, and keeps every buffer's capacity.
    d.maxWidth = 120.f;
    layoutText(f, *fullShaper(), mixed, d, L);
    const ShapeCacheStats s0 = shapeCacheStats(f);
    const auto gcap = L.glyphs.capacity(), lcap = L.lines.capacity();
    for (int r = 0; r < 50; ++r) layoutText(f, *fullShaper(), mixed, d, L);
    const ShapeCacheStats s1 = shapeCacheStats(f);
    UI_CHECK(s1.misses == s0.misses && s1.hits >= s0.hits + 150);
    UI_CHECK(L.glyphs.capacity() == gcap && L.lines.capacity() == lcap);
    // Thrash the cache with distinct runs, then the original still lays out identically (entries revalidated).
    std::vector<std::uint32_t> before = clusters(L.lines[0]);
    char buf[32];
    for (int r = 0; r < 5000; ++r) {
        std::snprintf(buf, sizeof buf, "w%d", r);
        layoutText(f, *fullShaper(), buf, d, L);
    }
    layoutText(f, *fullShaper(), mixed, d, L);
    UI_CHECK(clusters(L.lines[0]) == before);
#else
    UI_CHECK(textProfile() == TextProfile::Minimal);
    UI_CHECK(fullShaper() == nullptr);
    UI_CHECK(HarfBuzzShaper{}.shape(f, 16.f, "Hello", a) == MinimalShaper{}.shape(f, 16.f, "Hello", b));
#endif
    return fuse::ui::test::finish("fuse_ui_text_full");
}
