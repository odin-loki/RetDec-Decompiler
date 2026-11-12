// FUSE UI GPU backend, CPU side (no Vulkan types): packs a DrawList into the buffers the "ui.draw_list" kernel reads
// (src/shaders/ui_draw_list.comp). Clip stacks are resolved per command; commands are binned into 16x16 tiles in draw
// order. Every vector keeps its capacity across frames, so packing a frame of the same size does not allocate.
#pragma once

#include "fuse/ui/draw_list.hpp"

#include <vector>

namespace fuse::ui::render {

/// One command (std430, 128 bytes) — must match `GpuCmd` in ui_draw_list.comp.
struct GpuCmd {
    f32 rect[4];
    f32 fill[4];
    f32 fill2[4];
    f32 border[4];
    f32 radius[4];
    f32 clip[4];
    f32 params[4]; ///< borderWidth, blur, clipRadius, 0
    u32 info[4];   ///< op | gradient << 8, u | v << 16, tw | th << 16, image
};
static_assert(sizeof(GpuCmd) == 128, "GpuCmd must match the kernel's std430 layout");

/// Push constants (must match `UiPush`).
struct GpuPush {
    u32 extent[2];
    u32 tilesX;
    u32 atlasSize;
    u32 tileCount;
    u32 imageCount;
    u32 pad[2];
};

inline constexpr u32 kUiTile = 16;

struct GpuDrawData {
    std::vector<GpuCmd> cmds;
    std::vector<u32> tiles;  ///< offsets[tileCount + 1], then command ids
    std::vector<u32> images; ///< per image: offset (in u32 pixels into `pixels`), width, height, 0; then pixels
    GpuPush push{};
    const u8* atlas = nullptr; ///< glyph coverage atlas (atlasSize^2 bytes), uploaded as-is
    u32 atlasSize = 0;

    std::size_t cmdBytes() const { return cmds.size() * sizeof(GpuCmd); }
    std::size_t tileBytes() const { return tiles.size() * 4; }
    std::size_t imageBytes() const { return images.size() * 4; }
    std::size_t atlasBytes() const { return std::size_t(atlasSize) * atlasSize; }
};

/// Packs `list` for a width x height target, restricted to `scissor` (whole target when empty). `images` are the
/// rasteriser image views (same indices as DrawCmd::image); `atlas` / `atlasSize` the glyph atlas.
void packDrawList(const DrawList& list, u32 width, u32 height, GpuDrawData& out, const u8* atlas = nullptr,
                  u32 atlasSize = 0, const Image* const* images = nullptr, u32 imageCount = 0, Rect scissor = {});

} // namespace fuse::ui::render
