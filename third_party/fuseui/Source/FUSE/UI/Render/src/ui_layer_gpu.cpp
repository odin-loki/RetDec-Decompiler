// "ui.draw_list" render-graph pass: see include/fuse/ui/render/ui_layer_gpu.hpp.
#if defined(FUSE_HAS_VULKAN_RHI)

#include "fuse/ui/render/ui_layer_gpu.hpp"

#include <fuse/renderer/vk/device.hpp>

#include <vulkan/vulkan.h>

namespace fuse::ui::render {

bool UiLayerGpu::init(renderer::VulkanDevice* device, UiVkDesc limits) {
    if (device == nullptr || !device->isValid() || device->nativeHandle() == nullptr) return false;
    limits.device = device->nativeHandle();
    limits.physicalDevice = device->nativePhysicalDevice();
    if (limits.framesInFlight == 0 || limits.framesInFlight > UiVkRenderer::kMaxSlots)
        limits.framesInFlight = UiVkRenderer::kMaxSlots;
    m_frames = limits.framesInFlight;
    return m_vk.init(limits);
}

rg::TextureRef UiLayerGpu::createTarget(rg::Graph& graph, u32 width, u32 height) {
    rg::ImageDesc d{};
    d.width = width;
    d.height = height;
    d.format = 37u; // VK_FORMAT_R8G8B8A8_UNORM, the kernel's rgba8 image2D
    d.name = "ui.draw_list.target";
    return graph.createImage(d);
}

rg::TextureRef UiLayerGpu::addPass(rg::Graph& graph, rg::TextureRef target, const GpuDrawData& data) {
    if (!valid() || !m_vk.upload(m_slot, data)) return {};
    m_record = Record{this, target, m_slot};
    graph.addPass("ui.draw_list", &UiLayerGpu::record, &m_record)
        .use(target, rg::Access::StorageReadWrite, {}, rg::kStageCompute);
    return target;
}

void UiLayerGpu::record(const rg::PassContext& context, void* user) {
    const Record& r = *static_cast<const Record*>(user);
    r.self->m_vk.record(context.commandBuffer, context.imageView(r.target), r.slot);
}

#if defined(FUSE_UI_HAS_FRAME)
rg::TextureRef UiFrameSource::recordFrame(rg::Graph& graph, const renderer::frame::FrameUiContext& context, void* user) {
    auto& self = *static_cast<UiFrameSource*>(user);
    if (!self.valid() || self.m_list == nullptr || self.m_list->size() == 0 || context.width == 0 || context.height == 0)
        return {};
    packDrawList(*self.m_list, context.width, context.height, self.m_data, self.m_atlas, self.m_atlasSize);
    const GpuDrawData* d = &self.m_data;
    self.m_layer.beginFrame(context.serial);
    self.m_target = UiLayerGpu::createTarget(graph, context.width, context.height);
    graph.addPass("ui.draw_list.clear", &UiFrameSource::clear, &self).use(self.m_target, rg::Access::TransferDst);
    const rg::TextureRef out = self.m_layer.addPass(graph, self.m_target, *d);
    if (out.valid()) ++self.m_frames;
    return out;
}

void UiFrameSource::clear(const rg::PassContext& context, void* user) {
    const auto& self = *static_cast<const UiFrameSource*>(user);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(static_cast<VkCommandBuffer>(context.commandBuffer),
                         static_cast<VkImage>(context.image(self.m_target)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &zero, 1, &range);
}
#endif // defined(FUSE_UI_HAS_FRAME)

} // namespace fuse::ui::render

#endif // defined(FUSE_HAS_VULKAN_RHI)
