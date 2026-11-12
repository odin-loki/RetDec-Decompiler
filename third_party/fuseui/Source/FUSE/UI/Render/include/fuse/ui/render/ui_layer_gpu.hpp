#pragma once
// "ui.draw_list" on the render graph: the UiVkRenderer kernel as one compute pass that blends a packed UI draw list
// onto an RGBA8 (R8G8B8A8_UNORM) graph image with StorageReadWrite access (composite over the scene / sprite layer, or
// a cleared overlay). Mirrors hybrid::SpriteLayerGpu: per-ring-slot host-mapped buffers, no steady-state allocations.
#if defined(FUSE_HAS_VULKAN_RHI)

#include "fuse/ui/render/ui_vk.hpp"

#if defined(FUSE_UI_HAS_FRAME)
#include <fuse/renderer/frame/frame_composer.hpp>
#endif
#include <fuse/renderer/rg/graph.hpp>

namespace fuse::renderer {
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::ui::render {

namespace rg = ::fuse::renderer::rg;

class UiLayerGpu {
public:
    UiLayerGpu() = default;
    UiLayerGpu(const UiLayerGpu&) = delete;
    UiLayerGpu& operator=(const UiLayerGpu&) = delete;

    /// False without a Vulkan device / the embedded kernel (the stub backend always fails).
    bool init(renderer::VulkanDevice* device, UiVkDesc limits = {});
    void destroy() { m_vk.destroy(); }
    bool valid() const { return m_vk.valid(); }

    /// Selects the ring slot of the frame (serial % framesInFlight).
    void beginFrame(u64 serial) { m_slot = u32(serial % m_frames); }
    /// Uploads `data` into the slot and records "ui.draw_list" over `target` (an RGBA8 storage-capable image of
    /// data.push extent). Returns `target`; invalid when not valid() or the upload overflowed the limits.
    rg::TextureRef addPass(rg::Graph& graph, rg::TextureRef target, const GpuDrawData& data);
    /// Convenience: a new RGBA8 transient for the overlay. Its contents are undefined until a clear pass writes it.
    static rg::TextureRef createTarget(rg::Graph& graph, u32 width, u32 height);

private:
    struct Record {
        UiLayerGpu* self = nullptr;
        rg::TextureRef target;
        u32 slot = 0;
    };
    static void record(const rg::PassContext& context, void* user);

    UiVkRenderer m_vk;
    Record m_record{};
    u32 m_slot = 0;
    u32 m_frames = 1;
};

#if defined(FUSE_UI_HAS_FRAME)
/// The FUSE UI as the runtime UI stage (E02 frame::FrameUiSource): each frame it creates an RGBA8 transient of the
/// output extent, clears it to transparent, runs "ui.draw_list" over it with the draw data set for the frame and hands
/// it to the composer, which alpha-overs it onto the HUD-less output (frame.ui_composite) and gives it to frame
/// generation. No steady-state allocations (the layer's ring slots are preallocated; packing keeps capacity).
///
///   UiFrameSource ui;  ui.init(&device);  composer.setUiSource(ui.source());
///   per frame: build `list`;  ui.setDrawList(&list, atlas, atlasSize);  ... composer.addFrame(graph) ...
/// The list is packed at the composer's output extent inside the callback (so it follows resizes).
class UiFrameSource {
public:
    bool init(renderer::VulkanDevice* device, UiVkDesc limits = {}) { return m_layer.init(device, limits); }
    void destroy() { m_layer.destroy(); }
    bool valid() const { return m_layer.valid(); }
    /// Draw list of the next frame(s) (must stay alive until the graph is built); null or empty: no UI image.
    void setDrawList(const DrawList* list, const u8* atlas = nullptr, u32 atlasSize = 0) {
        m_list = list;
        m_atlas = atlas;
        m_atlasSize = atlasSize;
    }
    renderer::frame::FrameUiSource source() { return {&UiFrameSource::recordFrame, this}; }
    u32 frames() const { return m_frames; }
    UiLayerGpu& layer() { return m_layer; }

private:
    static rg::TextureRef recordFrame(rg::Graph& graph, const renderer::frame::FrameUiContext& context, void* user);
    static void clear(const rg::PassContext& context, void* user);

    UiLayerGpu m_layer;
    const DrawList* m_list = nullptr;
    const u8* m_atlas = nullptr;
    u32 m_atlasSize = 0;
    GpuDrawData m_data; // packed per frame; keeps its capacity
    rg::TextureRef m_target;
    u32 m_frames = 0;
};
#endif // defined(FUSE_UI_HAS_FRAME)

} // namespace fuse::ui::render

#endif // defined(FUSE_HAS_VULKAN_RHI)
