// FULL text profile (FUSE UI plan §4.8, decision D2): HarfBuzz shaping and FreeType rasterisation.
// Shaping (ligatures, GPOS kerning, marks, complex scripts), unhinted grayscale FreeType coverage, bidi (UAX #9 through
// SheenBidi: levels per byte, runs shaped per direction, L1 + L2 per line), UAX #14 line breaking (libunibreak) and a
// fixed-capacity shaped-run cache.
//
// Allocation: the run cache, its pools and the layout scratch live in FullFace and keep their capacity, so relaying
// out text already seen allocates nothing. SheenBidi allocates its paragraph objects; it only runs when the text
// holds a right-to-left code point (U+0590..U+08FF, U+FB1D..U+FDFF, U+FE70..U+FEFF, RLE/RLO/RLI controls), so
// left-to-right text never reaches it.
#include "text_full.hpp"

#include <hb.h>
extern "C" {
#include <SheenBidi.h>
#include <linebreak.h>
}
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>
#include <algorithm>
#include <cstring>

namespace fuse::text::detail {

namespace {
FT_Library ftLibrary() {
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        if (FT_Init_FreeType(&l) != 0) return FT_Library(nullptr);
        return l;
    }();
    return lib;
}
} // namespace

struct FullFace {
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    hb_buffer_t* buffer = nullptr; // reused: no allocation once it has grown
    FT_Face ft = nullptr;
    FT_F26Dot6 ftSize = -1;        // last FT_Set_Char_Size (26.6)

    // ---- shaped-run cache: direct-mapped entries over two ring pools (glyphs in font units, key bytes) ----
    struct UnitGlyph {
        std::uint32_t glyph, cluster; // cluster relative to the run start
        std::int32_t xOffset, advance;
    };
    struct Entry {
        std::uint64_t hash = 0, glyphStart = 0, byteStart = 0;
        std::uint32_t glyphCount = 0, len = 0;
        bool rtl = false, used = false;
    };
    static constexpr std::size_t kEntries = 1024, kGlyphPool = 16384, kBytePool = 65536;
    std::vector<Entry> entries = std::vector<Entry>(kEntries);
    std::vector<UnitGlyph> glyphPool = std::vector<UnitGlyph>(kGlyphPool);
    std::vector<char> bytePool = std::vector<char>(kBytePool);
    std::uint64_t glyphWrite = 0, byteWrite = 0, hits = 0, misses = 0;

    // ---- layout scratch (capacity kept) ----
    std::vector<std::uint8_t> breaks, levels, paraLevels, glyphLevels;
    std::vector<ShapedGlyph> lineTmp;
    std::vector<std::uint8_t> levelTmp;
};

FullFace* fullCreate(std::span<const std::uint8_t> bytes) {
    auto* f = new FullFace;
    f->blob = hb_blob_create(reinterpret_cast<const char*>(bytes.data()), unsigned(bytes.size()),
                             HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    f->face = hb_face_create(f->blob, 0);
    f->font = hb_font_create(f->face); // scale defaults to upem: positions come back in font units
    f->buffer = hb_buffer_create();
    FT_Library lib = ftLibrary();
    if (!lib || FT_New_Memory_Face(lib, bytes.data(), FT_Long(bytes.size()), 0, &f->ft) != 0 ||
        !hb_buffer_allocation_successful(f->buffer)) {
        fullDestroy(f);
        return nullptr;
    }
    return f;
}

void fullCacheStats(const FullFace* f, std::uint64_t& hits, std::uint64_t& misses) {
    hits = f ? f->hits : 0;
    misses = f ? f->misses : 0;
}

void fullDestroy(FullFace* f) {
    if (!f) return;
    if (f->ft) FT_Done_Face(f->ft);
    hb_buffer_destroy(f->buffer);
    hb_font_destroy(f->font);
    hb_face_destroy(f->face);
    hb_blob_destroy(f->blob);
    delete f;
}

float fullShape(FullFace* f, float unitScale, std::string_view s, std::vector<ShapedGlyph>& out) {
    out.clear();
    hb_buffer_clear_contents(f->buffer);
    hb_buffer_add_utf8(f->buffer, s.data(), int(s.size()), 0, int(s.size()));
    hb_buffer_guess_segment_properties(f->buffer);
    hb_shape(f->font, f->buffer, nullptr, 0);
    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(f->buffer, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(f->buffer, &n);
    float pen = 0.f;
    for (unsigned i = 0; i < n; ++i) {
        const float adv = float(pos[i].x_advance) * unitScale;
        out.push_back({info[i].codepoint, info[i].cluster, pen + float(pos[i].x_offset) * unitScale, adv});
        pen += adv;
    }
    return pen;
}

namespace {

std::uint64_t hashRun(std::string_view s, bool rtl) {
    std::uint64_t h = rtl ? 0x9E3779B97F4A7C15ull : 0xcbf29ce484222325ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

bool entryValid(const FullFace* f, const FullFace::Entry& e) {
    return e.used && f->glyphWrite - e.glyphStart <= FullFace::kGlyphPool &&
           f->byteWrite - e.byteStart <= FullFace::kBytePool;
}

bool entryMatches(const FullFace* f, const FullFace::Entry& e, std::string_view s, bool rtl, std::uint64_t h) {
    if (!entryValid(f, e) || e.hash != h || e.len != s.size() || e.rtl != rtl) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (f->bytePool[(e.byteStart + i) % FullFace::kBytePool] != s[i]) return false;
    return true;
}

/// Shapes bytes [begin, begin+len) of `s` (the whole paragraph is context) in one direction; appends glyphs in logical
/// order (clusters ascending) with absolute byte clusters, x = 0 and advance in pixels. Cached by (bytes, direction).
void shapeRun(FullFace* f, float unitScale, std::string_view s, std::uint32_t begin, std::uint32_t len, bool rtl,
              std::vector<ShapedGlyph>& out) {
    const std::string_view run = s.substr(begin, len);
    const std::uint64_t h = hashRun(run, rtl);
    FullFace::Entry& e = f->entries[h % FullFace::kEntries];
    if (!entryMatches(f, e, run, rtl, h)) {
        ++f->misses;
        hb_buffer_clear_contents(f->buffer);
        // The run alone: the cache key is the run's bytes, so shaping must not depend on the surrounding context.
        hb_buffer_add_utf8(f->buffer, run.data(), int(run.size()), 0, int(run.size()));
        hb_buffer_guess_segment_properties(f->buffer);
        hb_buffer_set_direction(f->buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_shape(f->font, f->buffer, nullptr, 0);
        unsigned n = 0;
        const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(f->buffer, &n);
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(f->buffer, &n);
        const bool fits = n <= FullFace::kGlyphPool / 4 && run.size() <= FullFace::kBytePool / 4;
        if (!fits) { // too large to cache: emit directly
            for (unsigned k = 0; k < n; ++k) {
                const unsigned i = rtl ? n - 1 - k : k;
                out.push_back({info[i].codepoint, begin + info[i].cluster, float(pos[i].x_offset) * unitScale,
                               float(pos[i].x_advance) * unitScale});
            }
            return;
        }
        e = {};
        e.hash = h;
        e.len = std::uint32_t(run.size());
        e.rtl = rtl;
        e.used = true;
        e.glyphStart = f->glyphWrite;
        e.glyphCount = n;
        e.byteStart = f->byteWrite;
        for (unsigned k = 0; k < n; ++k) { // logical order: HarfBuzz returns RTL runs in visual (reversed) order
            const unsigned i = rtl ? n - 1 - k : k;
            f->glyphPool[(f->glyphWrite++) % FullFace::kGlyphPool] = {info[i].codepoint, info[i].cluster,
                                                                        pos[i].x_offset, pos[i].x_advance};
        }
        for (char c : run) f->bytePool[(f->byteWrite++) % FullFace::kBytePool] = c;
    } else {
        ++f->hits;
    }
    for (std::uint32_t k = 0; k < e.glyphCount; ++k) {
        const auto& u = f->glyphPool[(e.glyphStart + k) % FullFace::kGlyphPool];
        out.push_back({u.glyph, begin + u.cluster, float(u.xOffset) * unitScale, float(u.advance) * unitScale});
    }
}

bool hasRtl(std::string_view s) {
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0xD6 && c <= 0xDF) return true; // U+0580..U+07FF (Hebrew, Arabic, Syriac, Thaana, NKo); U+0580 harmless
        if (c == 0xE0 && i + 1 < s.size()) {
            const unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0xA0 && d <= 0xA3) return true; // U+0800..U+08FF
        }
        if (c == 0xEF && i + 1 < s.size()) {
            const unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0xAC && d <= 0xBB) return true; // U+FB00..U+FEFF (presentation forms; U+FB00..1C harmless)
        }
        if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80) {
            const unsigned char d = static_cast<unsigned char>(s[i + 2]);
            if (d == 0x8F || d == 0xAB || d == 0xAE) return true; // RLM, RLE, RLO
        }
        if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x81 &&
            static_cast<unsigned char>(s[i + 2]) == 0xA7)
            return true; // RLI
    }
    return false;
}

bool isSpaceByte(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

} // namespace

void fullLayout(FullFace* f, float unitScale, std::string_view s, float maxWidth, float lineHeight, float ascent,
                std::vector<ShapedGlyph>& g, std::vector<TextLine>& lines) {
    g.clear();
    lines.clear();
    const std::size_t n = s.size();
    static const bool lbInit = [] { init_linebreak(); return true; }();
    (void)lbInit;
    f->breaks.resize(n);
    if (n) set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(s.data()), n, nullptr,
                               reinterpret_cast<char*>(f->breaks.data()));
    f->levels.assign(n, 0);
    f->paraLevels.assign(n, 0);
    if (n && hasRtl(s)) {
        SBCodepointSequence seq{SBStringEncodingUTF8, const_cast<char*>(s.data()), SBUInteger(n)};
        SBAlgorithmRef alg = SBAlgorithmCreate(&seq);
        SBUInteger off = 0;
        while (alg && off < n) {
            SBParagraphRef para = SBAlgorithmCreateParagraph(alg, off, SBUInteger(n - off), SBLevelDefaultLTR);
            if (!para) break;
            const SBUInteger plen = SBParagraphGetLength(para);
            const SBLevel* lv = SBParagraphGetLevelsPtr(para);
            const SBLevel base = SBParagraphGetBaseLevel(para);
            for (SBUInteger k = 0; k < plen; ++k) {
                f->levels[off + k] = lv[k];
                f->paraLevels[off + k] = base;
            }
            SBParagraphRelease(para);
            if (plen == 0) break;
            off += plen;
        }
        if (alg) SBAlgorithmRelease(alg);
    }

    // Shape level runs (never across a newline) in logical order.
    f->glyphLevels.clear();
    std::uint32_t i = 0;
    while (i < n) {
        std::uint32_t j = i + 1;
        if (s[i] != '\n')
            while (j < n && s[j] != '\n' && f->levels[j] == f->levels[i]) ++j;
        const std::size_t before = g.size();
        shapeRun(f, unitScale, s, i, j - i, (f->levels[i] & 1) != 0, g);
        f->glyphLevels.resize(g.size(), f->levels[i]);
        (void)before;
        i = j;
    }

    // Break lines over the logical glyph sequence.
    const std::uint32_t gn = std::uint32_t(g.size());
    const bool wrap = maxWidth > 0.f;
    auto canBreakBefore = [&](std::uint32_t k) {
        if (k == 0 || k >= gn || g[k].cluster == g[k - 1].cluster) return false;
        const std::uint32_t c = g[k].cluster;
        return c > 0 && f->breaks[c - 1] == LINEBREAK_ALLOWBREAK;
    };
    auto emit = [&](std::uint32_t b, std::uint32_t e) {
        TextLine ln;
        ln.firstGlyph = b;
        ln.glyphCount = e - b;
        std::uint32_t lo = std::uint32_t(n), hi = 0;
        for (std::uint32_t k = b; k < e; ++k) {
            lo = std::min(lo, g[k].cluster);
            hi = std::max(hi, g[k].cluster);
        }
        ln.byteBegin = b < e ? lo : (b < gn ? g[b].cluster : std::uint32_t(n));
        ln.byteEnd = e < gn ? g[e].cluster : std::uint32_t(n);
        if (b < e && hi >= ln.byteEnd) ln.byteEnd = std::uint32_t(n);
        const std::uint8_t para = b < e ? f->paraLevels[g[b].cluster] : 0;
        ln.rtl = (para & 1) != 0;
        // L1: trailing whitespace takes the paragraph level and does not count toward the width.
        std::uint32_t last = e;
        float trailing = 0.f;
        while (last > b && isSpaceByte(s[g[last - 1].cluster])) {
            --last;
            f->glyphLevels[last] = para;
            trailing += g[last].advance;
        }
        // L2: reverse every maximal sequence at or above each level, from the highest down to the lowest odd one.
        std::uint8_t hiLv = 0, loOdd = 0xff;
        for (std::uint32_t k = b; k < e; ++k) {
            hiLv = std::max(hiLv, f->glyphLevels[k]);
            if (f->glyphLevels[k] & 1) loOdd = std::min(loOdd, f->glyphLevels[k]);
        }
        for (int lv = hiLv; loOdd != 0xff && lv >= loOdd; --lv) {
            std::uint32_t k = b;
            while (k < e) {
                if (f->glyphLevels[k] < lv) { ++k; continue; }
                std::uint32_t m = k;
                while (m < e && f->glyphLevels[m] >= lv) ++m;
                std::reverse(g.begin() + k, g.begin() + m);
                std::reverse(f->glyphLevels.begin() + k, f->glyphLevels.begin() + m);
                k = m;
            }
        }
        // Pen positions in visual order; trailing whitespace of an RTL line hangs off the left edge.
        float pen = ln.rtl ? -trailing : 0.f;
        for (std::uint32_t k = b; k < e; ++k) {
            const float adv = g[k].advance;
            g[k].x += pen; // x held the glyph offset until now
            pen += adv;
        }
        float w = 0.f;
        for (std::uint32_t k = b; k < e; ++k) w += g[k].advance;
        ln.width = w - trailing;
        ln.baseline = float(lines.size()) * lineHeight + std::ceil(ascent);
        lines.push_back(ln);
    };
    std::uint32_t start = 0, lastBreak = 0;
    float lineW = 0.f;
    for (std::uint32_t k = 0; k < gn; ++k) {
        const char c = s[g[k].cluster];
        if (canBreakBefore(k)) lastBreak = k;
        if (c == '\n') {
            emit(start, k + 1);
            start = k + 1;
            lastBreak = 0;
            lineW = 0.f;
            continue;
        }
        if (wrap && !isSpaceByte(c) && k > start && lineW + g[k].advance > maxWidth) {
            const std::uint32_t at = lastBreak > start ? lastBreak : k;
            emit(start, at);
            start = at;
            lastBreak = 0;
            lineW = 0.f;
            k = at - 1; // re-measure from the break
            continue;
        }
        lineW += g[k].advance;
    }
    if (start < gn || lines.empty() || (gn > 0 && s[g[gn - 1].cluster] == '\n')) emit(start, gn);
    for (std::uint32_t k = 0; k < gn; ++k) if (s[g[k].cluster] == '\n' || s[g[k].cluster] == '\r') g[k].glyph = kInvisibleGlyph;
}

namespace {
bool loadRendered(FullFace* f, std::uint32_t glyph, float emPx) {
    const FT_F26Dot6 size = FT_F26Dot6(std::lround(emPx * 64.f));
    if (size != f->ftSize) {
        if (FT_Set_Char_Size(f->ft, 0, size, 72, 72) != 0) return false;
        f->ftSize = size;
    }
    if (FT_Load_Glyph(f->ft, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return false;
    return FT_Render_Glyph(f->ft->glyph, FT_RENDER_MODE_NORMAL) == 0;
}
} // namespace

void fullGlyphBox(FullFace* f, std::uint32_t glyph, float emPx, int& x0, int& y0, int& x1, int& y1) {
    x0 = y0 = x1 = y1 = 0;
    if (!loadRendered(f, glyph, emPx)) return;
    const FT_GlyphSlot g = f->ft->glyph;
    x0 = g->bitmap_left;
    y0 = -g->bitmap_top;
    x1 = x0 + int(g->bitmap.width);
    y1 = y0 + int(g->bitmap.rows);
}

void fullRasterise(FullFace* f, std::uint32_t glyph, float emPx, std::uint8_t* dst, int w, int h, int stride) {
    if (!loadRendered(f, glyph, emPx)) return;
    const FT_Bitmap& b = f->ft->glyph->bitmap;
    const int rows = std::min(h, int(b.rows)), cols = std::min(w, int(b.width));
    for (int y = 0; y < rows; ++y) std::memcpy(dst + y * stride, b.buffer + y * b.pitch, std::size_t(cols));
}

} // namespace fuse::text::detail
