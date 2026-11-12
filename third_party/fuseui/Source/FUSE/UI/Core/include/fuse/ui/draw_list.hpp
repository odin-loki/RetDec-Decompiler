// FUSE UI draw list (CPU data, no Vulkan types) and the CPU reference rasteriser. Plan §4.9.
//
// The CPU rasteriser is the specification of every primitive: fuse_ui_vk (UI4) is gated against it with FLIP.
// Output is premultiplied RGBA8 (R in the low byte) in display space, matching frame.ui_composite.
#pragma once

#include "fuse/ui/types.hpp"

#include <array>
#include <span>
#include <vector>

namespace fuse::ui {

enum class DrawOp : u8 {
    Rect,     ///< rounded rect: per-corner radii, optional border, optional 2-stop gradient; analytic AA
    Shadow,   ///< analytic box shadow (rounded rect blurred by `blur`)
    Glyph,    ///< coverage quad from the glyph atlas tinted by `fill`
    Image,    ///< RGBA8 premultiplied image registered with the rasteriser (`image` index), scaled to `rect`
    Line,     ///< `rect` = (x0, y0, x1, y1), width `borderWidth`
    PushClip, ///< intersect the clip with `rect` (rounded clips: radius[0])
    PopClip,
    Backdrop, ///< frosted glass: blurs what is already drawn under the rounded `rect` (radius `blur`), then `fill` over it
};

struct DrawCmd {
    DrawOp op = DrawOp::Rect;
    u8 gradient = 0; ///< 0 none, 1 vertical, 2 horizontal
    u16 u = 0, v = 0, tw = 0, th = 0; ///< Glyph: atlas texels
    u32 image = 0;
    u32 node = 0xFFFFFFFFu; ///< source node index (inspector, damage)
    Rect rect;
    Color fill, fill2, border;
    f32 borderWidth = 0.f;
    std::array<f32, 4> radius{};
    f32 blur = 0.f;
};

class DrawList {
public:
    void clear() { m_cmds.clear(); }
    void reserve(std::size_t n) { m_cmds.reserve(n); }
    void push(const DrawCmd& c) { m_cmds.push_back(c); }
    void rect(const Rect& r, Color fill, f32 radius = 0.f);
    void roundedRect(const Rect& r, Color fill, const std::array<f32, 4>& radii, Color border = {}, f32 borderWidth = 0.f);
    void gradientRect(const Rect& r, Color from, Color to, bool vertical, const std::array<f32, 4>& radii = {});
    void shadow(const Rect& r, Color c, f32 blur, f32 radius);
    void line(Vec2 a, Vec2 b, Color c, f32 width);
    /// Backdrop blur (CSS backdrop-filter: blur()): everything drawn before this command under `r` is blurred.
    void backdrop(const Rect& r, f32 blur, const std::array<f32, 4>& radii, Color tint = {});
    void pushClip(const Rect& r, f32 radius = 0.f);
    void popClip();
    const std::vector<DrawCmd>& cmds() const { return m_cmds; }
    std::vector<DrawCmd>& cmds() { return m_cmds; }
    std::size_t size() const { return m_cmds.size(); }
    /// FNV-64 over the commands (golden / parity hashes).
    u64 hash() const;
    /// Moves every command by `d` (Line end points included): re-bases a subtree's list to a popup window's origin.
    void translate(Vec2 d);

private:
    std::vector<DrawCmd> m_cmds;
};

/// A premultiplied RGBA8 image.
struct Image {
    int width = 0, height = 0;
    std::vector<u32> pixels;
    /// >= 0: on the GPU (fuse_ui_vk) path the image is the external texture bound to this slot
    /// (UiVkRenderer::setExternalImage) and `pixels` are only the CPU rasteriser's fallback / screenshot copy.
    int externalSlot = -1;
    void resize(int w, int h, u32 clear = 0) {
        width = w;
        height = h;
        pixels.assign(std::size_t(w) * std::size_t(h), clear);
    }
    u32 at(int x, int y) const { return pixels[std::size_t(y) * std::size_t(width) + std::size_t(x)]; }
    u64 hash() const;
};

/// Packs a straight colour to premultiplied RGBA8.
u32 packPremultiplied(Color c);

class CpuRasterizer {
public:
    /// Coverage atlas for Glyph commands (non-owning; must outlive render calls).
    void setGlyphAtlas(const u8* pixels, int size) {
        m_atlas = pixels;
        m_atlasSize = size;
    }
    /// Registers an image for Image commands (non-owning view). Returns its index.
    u32 addImage(const Image* img);
    /// Rasterises `list` over `target` (blending onto existing pixels) restricted to `scissor` (whole target when empty).
    void render(const DrawList& list, Image& target, Rect scissor = {}) const;
    /// The registered images, by index (what packDrawList needs for the GPU path).
    const Image* const* images() const { return m_images.data(); }
    u32 imageCount() const { return u32(m_images.size()); }

private:
    const u8* m_atlas = nullptr;
    int m_atlasSize = 0;
    std::vector<const Image*> m_images;
};

} // namespace fuse::ui
