// Internal: the FULL text profile backend (HarfBuzz shaping + FreeType rasterisation). Only compiled when
// FUSE_UI_TEXT=FULL (FUSE_TEXT_FULL=1); text.cpp calls through these hooks so the public API stays profile-neutral.
#pragma once

#include "fuse/text/text.hpp"

namespace fuse::text::detail {

struct FullFace; // hb_face_t + hb_font_t + FT_Face over the Font's byte copy

/// Creates the HarfBuzz / FreeType objects over `bytes` (which must outlive the result). nullptr on failure.
FullFace* fullCreate(std::span<const std::uint8_t> bytes);
void fullDestroy(FullFace* f);
/// Shapes `utf8`; `unitScale` converts font units to pixels. Clears and fills `out`; returns the run width.
float fullShape(FullFace* f, float unitScale, std::string_view utf8, std::vector<ShapedGlyph>& out);
/// FreeType bitmap box / coverage at an em size of `emPx` pixels (unhinted, grayscale).
void fullGlyphBox(FullFace* f, std::uint32_t glyph, float emPx, int& x0, int& y0, int& x1, int& y1);
/// FULL layout: bidi runs (SheenBidi) shaped per direction through the run cache, UAX #14 line breaking
/// (libunibreak), rule L1 trailing whitespace and L2 reordering per line. Fills `glyphs` (visual order per line, x from
/// the line start) and `lines` (baseline, width, rtl). `maxWidth` <= 0: no wrapping.
void fullLayout(FullFace* f, float unitScale, std::string_view utf8, float maxWidth, float lineHeight, float ascent,
                std::vector<ShapedGlyph>& glyphs, std::vector<TextLine>& lines);
void fullCacheStats(const FullFace* f, std::uint64_t& hits, std::uint64_t& misses);
void fullRasterise(FullFace* f, std::uint32_t glyph, float emPx, std::uint8_t* dst, int w, int h, int stride);

} // namespace fuse::text::detail
