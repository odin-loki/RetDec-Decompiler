#include "fuse/ui/render/gpu_draw_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::ui::render {

namespace {
struct Clip {
    Rect r;
    f32 radius;
};
void put4(f32 (&d)[4], f32 a, f32 b, f32 c, f32 e) {
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
}
} // namespace

void packDrawList(const DrawList& list, u32 width, u32 height, GpuDrawData& out, const u8* atlas, u32 atlasSize,
                  const Image* const* images, u32 imageCount, Rect scissor) {
    out.cmds.clear();
    out.tiles.clear();
    out.images.clear();
    out.atlas = atlas;
    out.atlasSize = atlas ? atlasSize : 0;
    const u32 tilesX = (width + kUiTile - 1) / kUiTile, tilesY = (height + kUiTile - 1) / kUiTile;
    const u32 tileCount = tilesX * tilesY;
    out.push = {{width, height}, tilesX, out.atlasSize, tileCount, imageCount, {0, 0}};

    // images: table then pixels
    out.images.resize(std::size_t(imageCount) * 4);
    for (u32 i = 0; i < imageCount; ++i) {
        const Image* img = images ? images[i] : nullptr;
        out.images[i * 4 + 0] = u32(out.images.size());
        out.images[i * 4 + 1] = img ? u32(img->width) : 1u;
        out.images[i * 4 + 2] = img ? u32(img->height) : 1u;
        // external GPU texture: the kernel samples the bound slot; the CPU pixels stay the rasteriser's fallback
        const bool ext = img && img->externalSlot >= 0 && img->externalSlot < 4;
        out.images[i * 4 + 3] = ext ? u32(img->externalSlot) + 1u : 0u;
        if (ext) {
            out.images.push_back(0u);
            continue;
        }
        if (img && !img->pixels.empty()) out.images.insert(out.images.end(), img->pixels.begin(), img->pixels.end());
        else out.images.push_back(0u);
    }
    if (out.images.empty()) out.images.push_back(0u); // a storage buffer may not be empty

    const Rect full{0.f, 0.f, f32(width), f32(height)};
    if (scissor.empty()) scissor = full;
    scissor = scissor.intersect(full);
    Clip stack[32];
    int depth = 0;
    stack[0] = {scissor, 0.f};

    // pass 1: commands + their pixel areas (counted per tile)
    out.tiles.assign(std::size_t(tileCount) + 1, 0u);
    struct Area {
        int x0, y0, x1, y1;
    };
    thread_local std::vector<Area> areas;
    areas.clear();
    for (const DrawCmd& c : list.cmds()) {
        if (c.op == DrawOp::PushClip) {
            if (depth < 31) {
                ++depth;
                stack[depth] = {stack[depth - 1].r.intersect(c.rect), c.radius[0]};
                if (stack[depth - 1].radius > 0.f && c.radius[0] <= 0.f) stack[depth].radius = 0.f;
            }
            continue;
        }
        if (c.op == DrawOp::PopClip) {
            if (depth > 0) --depth;
            continue;
        }
        if (c.op == DrawOp::Backdrop) continue; // single-pass tile kernel: no blur pass yet (panel drawn unblurred)
        const Clip& clip = stack[depth];
        Rect bounds = c.rect;
        if (c.op == DrawOp::Shadow)
            bounds = {c.rect.x - c.blur * 2.f, c.rect.y - c.blur * 2.f, c.rect.w + c.blur * 4.f, c.rect.h + c.blur * 4.f};
        if (c.op == DrawOp::Line) {
            const f32 hw = c.borderWidth * 0.5f + 1.f;
            bounds = {std::min(c.rect.x, c.rect.w) - hw, std::min(c.rect.y, c.rect.h) - hw,
                      std::abs(c.rect.w - c.rect.x) + 2 * hw, std::abs(c.rect.h - c.rect.y) + 2 * hw};
        }
        const Rect area = bounds.intersect(clip.r);
        if (area.empty()) continue;
        if (c.op == DrawOp::Glyph && !atlas) continue;
        if (c.op == DrawOp::Image && c.image >= imageCount) continue;
        GpuCmd g{};
        put4(g.rect, c.rect.x, c.rect.y, c.rect.w, c.rect.h);
        put4(g.fill, c.fill.r, c.fill.g, c.fill.b, c.fill.a);
        put4(g.fill2, c.fill2.r, c.fill2.g, c.fill2.b, c.fill2.a);
        put4(g.border, c.border.r, c.border.g, c.border.b, c.border.a);
        put4(g.radius, c.radius[0], c.radius[1], c.radius[2], c.radius[3]);
        put4(g.clip, clip.r.x, clip.r.y, clip.r.w, clip.r.h);
        put4(g.params, c.borderWidth, c.blur, clip.radius, 0.f);
        g.info[0] = u32(c.op) | (u32(c.gradient) << 8);
        g.info[1] = u32(c.u) | (u32(c.v) << 16);
        g.info[2] = u32(c.tw) | (u32(c.th) << 16);
        g.info[3] = c.image;
        out.cmds.push_back(g);
        const Area a{std::max(0, int(std::floor(area.x))), std::max(0, int(std::floor(area.y))),
                     std::min(int(width), int(std::ceil(area.right()))), std::min(int(height), int(std::ceil(area.bottom())))};
        areas.push_back(a);
        if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
        for (int ty = a.y0 / int(kUiTile); ty <= (a.y1 - 1) / int(kUiTile); ++ty)
            for (int tx = a.x0 / int(kUiTile); tx <= (a.x1 - 1) / int(kUiTile); ++tx) ++out.tiles[std::size_t(ty) * tilesX + tx];
    }
    // prefix sums -> offsets; then fill ids in draw order
    u32 sum = 0;
    for (u32 t = 0; t <= tileCount; ++t) {
        const u32 n = out.tiles[t];
        out.tiles[t] = sum;
        sum += t < tileCount ? n : 0;
    }
    out.tiles[tileCount] = sum;
    const std::size_t base = std::size_t(tileCount) + 1;
    out.tiles.resize(base + sum + 1, 0u); // +1: never empty
    thread_local std::vector<u32> cursor;
    cursor.assign(out.tiles.begin(), out.tiles.begin() + tileCount);
    for (u32 i = 0; i < areas.size(); ++i) {
        const Area& a = areas[i];
        if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
        for (int ty = a.y0 / int(kUiTile); ty <= (a.y1 - 1) / int(kUiTile); ++ty)
            for (int tx = a.x0 / int(kUiTile); tx <= (a.x1 - 1) / int(kUiTile); ++tx)
                out.tiles[base + cursor[std::size_t(ty) * tilesX + tx]++] = i;
    }
    if (out.cmds.empty()) out.cmds.push_back(GpuCmd{}); // non-empty buffer; no tile references it
}

} // namespace fuse::ui::render
