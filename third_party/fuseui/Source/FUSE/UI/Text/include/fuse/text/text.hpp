// fuse_text — fonts, shaping, line breaking, glyph cache and text editing (FUSE UI plan §4.8, phase UI3).
//
// Profiles (FUSE_UI_TEXT, decision D2 adopted 2026-10-29): FULL (HarfBuzz + FreeType + bidi + UAX #14/#29),
// MINIMAL (cmap + kerning shaping, stb_truetype grayscale rasterisation; Latin / Greek / Cyrillic / CJK, no complex
// scripts or bidi), COOKED (pre-baked .fusefont only). MINIMAL is the default; FULL (FUSE_UI_TEXT=FULL, fetched
// HarfBuzz + FreeType) swaps in HarfBuzzShaper and FreeType coverage behind the same interfaces. COOKED is not built
// yet. textProfile() reports what is compiled.
//
// Engine rules: no exceptions (std::expected), no owning raw pointers, no allocation on the steady-state path: shaping
// and layout write into caller-owned buffers that keep their capacity, and the glyph atlas only allocates on a miss.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::text {

enum class TextProfile : std::uint8_t { Minimal, Full, Cooked };
/// Profile compiled into this build (MINIMAL until FULL dependencies are vendored).
TextProfile textProfile();
const char* textProfileName(TextProfile p);

enum class TextError : std::uint8_t { InvalidFont, NoGlyphs, AtlasFull, InvalidUtf8 };
const char* textErrorName(TextError e);

// ---- UTF-8 ---------------------------------------------------------------------------------------------------------
/// Decodes one code point at `i` and advances it; invalid bytes decode as U+FFFD and advance one byte.
char32_t decodeUtf8(std::string_view s, std::size_t& i);
/// Appends the UTF-8 encoding of `cp` to `out`.
void appendUtf8(std::string& out, char32_t cp);
/// Byte offset of the previous / next code point boundary.
std::size_t prevCodepoint(std::string_view s, std::size_t i);
std::size_t nextCodepoint(std::string_view s, std::size_t i);
/// UAX #29 extended grapheme cluster property (compact built-in tables: combining marks, Indic spacing marks,
/// Hangul syllable types, regional indicators, emoji modifiers / ZWJ sequences, variation selectors, tags).
enum class GraphemeBreak : std::uint8_t { Other, CR, LF, Control, Extend, ZWJ, RegionalIndicator, Prepend, SpacingMark, L, V, T, LV, LVT, ExtPict };
GraphemeBreak graphemeBreakProperty(char32_t cp);
/// Byte offset of the previous / next extended grapheme cluster boundary (UAX #29 rules GB3-GB13): caret moves,
/// Backspace and Delete never split "e + U+0301", a flag, a skin-toned or ZWJ emoji, CR LF or a Hangul syllable.
std::size_t prevBoundary(std::string_view s, std::size_t i);
std::size_t nextBoundary(std::string_view s, std::size_t i);
/// Number of extended grapheme clusters in `s` (user-perceived characters).
std::size_t graphemeCount(std::string_view s);

// ---- Font ----------------------------------------------------------------------------------------------------------
struct FontMetrics {
    float ascent = 0.f, descent = 0.f, lineGap = 0.f; ///< pixels at the size asked for (descent is negative)
    float lineHeight() const { return ascent - descent + lineGap; }
};

/// One layer of a COLRv0 colour glyph: an ordinary outline glyph filled with a CPAL palette colour (straight RGBA),
/// or with the text colour when `foreground` is set. Layers paint bottom to top.
struct ColorLayer {
    std::uint32_t glyph = 0;
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    bool foreground = false;
};

class ITextShaper;
struct TextLayoutDesc;
struct TextLayout;
struct ShapeCacheStats;

/// A TrueType / OpenType (glyf) font. Holds a copy of the file bytes; the parsed state points into it.
class Font {
public:
    static std::expected<Font, TextError> fromBytes(std::span<const std::uint8_t> bytes);
    static std::expected<Font, TextError> fromFile(const std::string& path);
    /// FUSE's bundled UI font (vendor/fonts/opensans) when the build knows its path; InvalidFont otherwise.
    static std::expected<Font, TextError> bundled();
    /// FUSE's bundled fallback faces in chain order: Noto Sans Hebrew, Arabic, Devanagari, Symbols 2 and Twemoji
    /// (COLRv0 colour emoji). Fonts whose files are missing are skipped. Attach them with setFallbacks().
    static std::vector<Font> bundledFallbacks();

    Font() = default;
    Font(Font&&) noexcept;
    Font& operator=(Font&&) noexcept;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    ~Font();

    bool valid() const { return m_state != nullptr; }
    FontMetrics metrics(float pixelSize) const;
    std::uint32_t glyphIndex(char32_t cp) const;
    /// Horizontal advance and kerning in pixels.
    float advance(std::uint32_t glyph, float pixelSize) const;
    float kerning(std::uint32_t left, std::uint32_t right, float pixelSize) const;
    /// Bitmap box of a glyph at `pixelSize` (x0,y0 relative to the pen on the baseline; y down).
    void glyphBox(std::uint32_t glyph, float pixelSize, int& x0, int& y0, int& x1, int& y1) const;
    /// Rasterises 8-bit coverage into `dst` (row stride `stride`) of size w*h.
    void rasterise(std::uint32_t glyph, float pixelSize, std::uint8_t* dst, int w, int h, int stride) const;
    /// Stable id for caches (unique per loaded font in this process).
    std::uint32_t id() const { return m_id; }
    /// Font units to pixels at `pixelSize`.
    float unitScale(float pixelSize) const;

    // ---- Fallback chain (Qt: QFont family substitution) ----
    /// Faces tried in order for code points this font has no glyph for (non-owning; they must outlive this font).
    /// Layout splits text into runs per face at grapheme-cluster granularity, so a base and its marks, emoji modifier
    /// and ZWJ sequences stay on one face.
    void setFallbacks(std::vector<const Font*> chain);
    /// 1 + the number of fallbacks; face 0 is this font.
    std::size_t faceCount() const;
    const Font& face(std::uint16_t i) const;
    /// First face (0 = this) whose cmap maps `cp`; 0 when no face has it (draws this font's .notdef).
    std::uint16_t faceFor(char32_t cp) const;
    /// Pixel size to use for face `i` so its em matches this font's em at `pixelSize`.
    float facePixelSize(std::uint16_t i, float pixelSize) const;

    // ---- Colour glyphs (COLRv0 + CPAL, palette 0) ----
    bool hasColorGlyphs() const;
    /// Layers of `glyph` when it is a colour glyph; empty for ordinary glyphs.
    std::span<const ColorLayer> colorLayers(std::uint32_t glyph) const;

private:
    friend class HarfBuzzShaper;
    friend void layoutText(const Font&, const ITextShaper&, std::string_view, const TextLayoutDesc&, TextLayout&);
    friend ShapeCacheStats shapeCacheStats(const Font&);
    struct State;
    State* m_state = nullptr; // owned (pimpl); released in ~Font
    std::uint32_t m_id = 0;
};

// ---- Shaping -------------------------------------------------------------------------------------------------------
/// Glyph id given to line-break characters after layout: they break the line but are never drawn.
inline constexpr std::uint32_t kInvisibleGlyph = 0xFFFFFFFFu;

struct ShapedGlyph {
    std::uint32_t glyph = 0;
    std::uint32_t cluster = 0; ///< byte offset of the source code point
    float x = 0.f;             ///< pen position (pixels) relative to the run start
    float advance = 0.f;
    std::uint16_t face = 0;    ///< 0: the layout font; k: its fallback k (Font::face)
};

/// Shaper interface (FULL: HarfBuzz; MINIMAL: cmap + kerning).
class ITextShaper {
public:
    virtual ~ITextShaper() = default;
    /// Shapes `utf8` with `font` at `pixelSize`; clears and fills `out` (keeps its capacity). Returns the run width.
    virtual float shape(const Font& font, float pixelSize, std::string_view utf8, std::vector<ShapedGlyph>& out) const = 0;
};

class MinimalShaper final : public ITextShaper {
public:
    float shape(const Font& font, float pixelSize, std::string_view utf8, std::vector<ShapedGlyph>& out) const override;
};

/// HarfBuzz shaping (FULL profile): ligatures, GPOS kerning, mark positioning, complex scripts. Glyphs come back in
/// visual order for one direction run; bidi reordering is not wired yet. Falls back to MinimalShaper when FULL is not
/// compiled in.
class HarfBuzzShaper final : public ITextShaper {
public:
    float shape(const Font& font, float pixelSize, std::string_view utf8, std::vector<ShapedGlyph>& out) const override;
};
/// The HarfBuzz shaper when FULL is compiled in, nullptr otherwise.
const ITextShaper* fullShaper();

/// The shaper of the compiled profile.
const ITextShaper& defaultShaper();

// ---- Line breaking and layout --------------------------------------------------------------------------------------
struct TextLine {
    std::uint32_t firstGlyph = 0, glyphCount = 0;
    std::uint32_t byteBegin = 0, byteEnd = 0;
    float width = 0.f;
    float baseline = 0.f; ///< y of the baseline from the top of the block
    bool rtl = false;     ///< paragraph base direction is right-to-left (FULL bidi); Start alignment then means right
};

enum class TextAlign : std::uint8_t { Start, Center, End };

struct TextLayoutDesc {
    float pixelSize = 14.f;
    float maxWidth = 0.f; ///< <= 0: no wrapping
    TextAlign align = TextAlign::Start;
    bool elide = false; ///< single line clipped to maxWidth with an ellipsis
};

/// A shaped, line-broken block. Reuse one object per text node: relayout keeps every buffer's capacity.
struct TextLayout {
    std::vector<ShapedGlyph> glyphs; ///< x already includes line alignment; y comes from the line
    std::vector<TextLine> lines;
    float width = 0.f, height = 0.f;
    FontMetrics metrics;
};

/// Shapes and breaks `utf8` (break opportunities after spaces and at hard newlines, UAX #14 subset; words longer than
/// the line break at a glyph). Clears and fills `out`.
void layoutText(const Font& font, const ITextShaper& shaper, std::string_view utf8, const TextLayoutDesc& desc,
                TextLayout& out);
/// FULL profile: bidi runs (UAX #9, SheenBidi) are shaped per direction, lines break at UAX #14 opportunities
/// (libunibreak) and each line is reordered visually (rule L2), so `glyphs` of a line are in visual order with
/// increasing x. Shaped runs go through a fixed-capacity per-font cache (ring pools, no steady-state allocation).
struct ShapeCacheStats {
    std::uint64_t hits = 0, misses = 0;
};
/// Run-cache counters of `font` (zeros in MINIMAL builds).
ShapeCacheStats shapeCacheStats(const Font& font);

/// Shapes one line with the default shaper and `font`'s fallback chain (glyph `face` set per run). Returns the width.
float shapeText(const Font& font, std::string_view utf8, float pixelSize, std::vector<ShapedGlyph>& out);

/// Calls fn(face, glyph, facePixelSize, layer) for every paint layer of a shaped glyph: once with layer == nullptr
/// (draw in the text colour) for an ordinary glyph, or once per COLRv0 layer bottom to top for a colour glyph.
template <class Fn> void forEachPaintGlyph(const Font& font, const ShapedGlyph& g, float pixelSize, Fn&& fn) {
    const Font& f = font.face(g.face);
    const float px = g.face ? font.facePixelSize(g.face, pixelSize) : pixelSize;
    const auto layers = f.colorLayers(g.glyph);
    if (layers.empty()) fn(f, g.glyph, px, static_cast<const ColorLayer*>(nullptr));
    else
        for (const ColorLayer& l : layers) fn(f, l.glyph, px, &l);
}

/// Width of a single line without wrapping (no allocation when `scratch` has capacity).
float measureText(const Font& font, std::string_view utf8, float pixelSize, std::vector<ShapedGlyph>& scratch);

// ---- Glyph atlas ---------------------------------------------------------------------------------------------------
struct AtlasGlyph {
    std::uint16_t u = 0, v = 0, w = 0, h = 0; ///< texels in the atlas page
    std::int16_t x0 = 0, y0 = 0;              ///< offset from the pen on the baseline
    bool valid = false;
};

/// One 8-bit coverage page with shelf packing and an open-addressed lookup (no allocation on hit).
/// When full, the page is reset and the generation bumps (callers re-request their glyphs: LRU by page).
class GlyphAtlas {
public:
    explicit GlyphAtlas(int size = 1024);
    /// Finds or rasterises `glyph` of `font` at `pixelSize` (sizes are bucketed to 1/4 px).
    AtlasGlyph get(const Font& font, std::uint32_t glyph, float pixelSize);
    int size() const { return m_size; }
    const std::uint8_t* pixels() const { return m_pixels.data(); }
    std::uint32_t generation() const { return m_generation; }
    std::uint32_t misses() const { return m_misses; }
    void clear();

private:
    struct Slot {
        std::uint64_t key = 0;
        AtlasGlyph g;
    };
    int m_size;
    std::vector<std::uint8_t> m_pixels;
    std::vector<Slot> m_slots;
    std::uint32_t m_used = 0;
    int m_shelfX = 1, m_shelfY = 1, m_shelfH = 0;
    std::uint32_t m_generation = 0;
    std::uint32_t m_misses = 0;
};

// ---- Editing -------------------------------------------------------------------------------------------------------
/// Single-line / multi-line edit buffer: cursor + anchor selection, boundary-aware movement, word jumps, undo / redo.
/// (Piece table per plan §4.8 is the FULL-profile structure; this MINIMAL buffer is a flat string with an undo log.)
class TextEditBuffer {
public:
    explicit TextEditBuffer(std::size_t maxLength = 4096) : m_maxLength(maxLength) {}
    const std::string& text() const { return m_text; }
    void setText(std::string_view s);
    std::size_t cursor() const { return m_cursor; }
    std::size_t anchor() const { return m_anchor; }
    bool hasSelection() const { return m_cursor != m_anchor; }
    std::string_view selection() const;

    void insert(std::string_view utf8); ///< replaces the selection
    void backspace();
    void del();
    void moveLeft(bool select);
    void moveRight(bool select);
    void wordLeft(bool select);
    void wordRight(bool select);
    void home(bool select);
    void end(bool select);
    void selectAll();
    bool undo();
    bool redo();
    std::size_t maxLength() const { return m_maxLength; }

private:
    struct Edit {
        std::string before, after;
        std::size_t cursorBefore, cursorAfter;
    };
    void replaceSelection(std::string_view s);
    void record(std::string before, std::size_t cursorBefore);
    std::string m_text;
    std::size_t m_cursor = 0, m_anchor = 0, m_maxLength;
    std::vector<Edit> m_undo, m_redo;
};

} // namespace fuse::text
