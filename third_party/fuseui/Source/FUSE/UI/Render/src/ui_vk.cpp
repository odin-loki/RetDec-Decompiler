#include "fuse/ui/render/ui_vk.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>

#if __has_include("ui_draw_list_spv.h")
#include "ui_draw_list_spv.h" // generated: kUiDrawListSpv
#define FUSE_UI_VK_HAS_KERNEL 1
#endif

namespace fuse::ui::render {

bool UiVkRenderer::kernelAvailable() {
#if defined(FUSE_UI_VK_HAS_KERNEL)
    return true;
#else
    return false;
#endif
}

namespace {
u64 alignUp(u64 v, u64 a) { return (v + a - 1) / a * a; }
} // namespace

bool UiVkRenderer::init(const UiVkDesc& desc) {
    destroy();
#if defined(FUSE_UI_VK_HAS_KERNEL)
    if (!desc.device || !desc.physicalDevice || desc.maxCmds == 0) return false;
    m_desc = desc;
    if (m_desc.framesInFlight == 0) m_desc.framesInFlight = 1;
    if (m_desc.framesInFlight > kMaxSlots) m_desc.framesInFlight = kMaxSlots;
    const VkDevice device = static_cast<VkDevice>(desc.device);
    const VkPhysicalDevice phys = static_cast<VkPhysicalDevice>(desc.physicalDevice);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    const u64 al = std::max<u64>(props.limits.minStorageBufferOffsetAlignment, 16);
    m_offsets[0] = 0;
    m_offsets[1] = alignUp(m_offsets[0] + u64(desc.maxCmds) * sizeof(GpuCmd), al);
    m_offsets[2] = alignUp(m_offsets[1] + u64(desc.maxTileWords) * 4, al);
    m_offsets[3] = alignUp(m_offsets[2] + alignUp(u64(desc.maxAtlasSize) * desc.maxAtlasSize, 4), al);
    m_offsets[4] = alignUp(m_offsets[3] + u64(desc.maxImageWords) * 4, al);

    constexpr u32 kBindings = 5 + kMaxExternal; // target, 4 buffers, external textures
    VkDescriptorSetLayoutBinding b[kBindings]{};
    for (u32 i = 0; i < kBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                              : i < 5 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                      : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = kBindings;
    sli.pBindings = b;
    VkDescriptorSetLayout setLayout{};
    if (vkCreateDescriptorSetLayout(device, &sli, nullptr, &setLayout) != VK_SUCCESS) return destroy(), false;
    m_setLayout = setLayout;
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuPush)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VkPipelineLayout layout{};
    if (vkCreatePipelineLayout(device, &pli, nullptr, &layout) != VK_SUCCESS) return destroy(), false;
    m_layout = layout;
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = sizeof(kUiDrawListSpv);
    mi.pCode = kUiDrawListSpv;
    VkShaderModule module{};
    if (vkCreateShaderModule(device, &mi, nullptr, &module) != VK_SUCCESS) return destroy(), false;
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline pipeline{};
    const VkResult pr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (pr != VK_SUCCESS) return destroy(), false;
    m_pipeline = pipeline;

    const u32 n = m_desc.framesInFlight;
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, n},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * n},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxExternal * n}};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = n;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    if (vkCreateDescriptorPool(device, &dpi, nullptr, &pool) != VK_SUCCESS) return destroy(), false;
    m_pool = pool;

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);

    // external texture slots: nearest sampler + a 1x1 dummy image for unbound slots
    VkSamplerCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    smi.magFilter = smi.minFilter = VK_FILTER_NEAREST;
    smi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smi.maxLod = 0.f;
    VkSampler sampler{};
    if (vkCreateSampler(device, &smi, nullptr, &sampler) != VK_SUCCESS) return destroy(), false;
    m_sampler = sampler;
    {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {1, 1, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img{};
        if (vkCreateImage(device, &ii, nullptr, &img) != VK_SUCCESS) return destroy(), false;
        m_dummy = img;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, img, &req);
        u32 type = ~0u;
        for (u32 t = 0; t < mem.memoryTypeCount && type == ~0u; ++t)
            if (req.memoryTypeBits & (1u << t)) type = t;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkDeviceMemory dm{};
        if (type == ~0u || vkAllocateMemory(device, &ai, nullptr, &dm) != VK_SUCCESS) return destroy(), false;
        m_dummyMem = dm;
        if (vkBindImageMemory(device, img, dm, 0) != VK_SUCCESS) return destroy(), false;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view{};
        if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) return destroy(), false;
        m_dummyView = view;
        m_dummyReady = false;
    }
    for (u32 s = 0; s < n; ++s) {
        Slot& slot = m_slots[s];
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = m_offsets[4];
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkBuffer buf{};
        if (vkCreateBuffer(device, &bi, nullptr, &buf) != VK_SUCCESS) return destroy(), false;
        slot.buffer = buf;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buf, &req);
        u32 type = ~0u;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (u32 t = 0; t < mem.memoryTypeCount && type == ~0u; ++t)
            if ((req.memoryTypeBits & (1u << t)) && (mem.memoryTypes[t].propertyFlags & want) == want) type = t;
        if (type == ~0u) return destroy(), false;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkDeviceMemory dm{};
        if (vkAllocateMemory(device, &ai, nullptr, &dm) != VK_SUCCESS) return destroy(), false;
        slot.memory = dm;
        if (vkBindBufferMemory(device, buf, dm, 0) != VK_SUCCESS) return destroy(), false;
        void* p = nullptr;
        if (vkMapMemory(device, dm, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return destroy(), false;
        slot.mapped = static_cast<u8*>(p);
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &setLayout;
        VkDescriptorSet set{};
        if (vkAllocateDescriptorSets(device, &dai, &set) != VK_SUCCESS) return destroy(), false;
        slot.set = set;
        // buffer bindings never change: write them once
        VkDescriptorBufferInfo infos[4];
        VkWriteDescriptorSet w[4]{};
        const u32 order[4] = {0, 1, 2, 3}; // binding 1 cmds, 2 tiles, 3 atlas, 4 images
        for (u32 i = 0; i < 4; ++i) {
            infos[i] = {buf, m_offsets[order[i]], m_offsets[order[i] + 1] - m_offsets[order[i]]};
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i + 1;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 4, w, 0, nullptr);
    }
    return true;
#else
    (void)desc;
    return false;
#endif
}

void UiVkRenderer::destroy() {
    if (!m_desc.device) return;
    const VkDevice device = static_cast<VkDevice>(m_desc.device);
    for (Slot& s : m_slots) {
        if (s.mapped) vkUnmapMemory(device, static_cast<VkDeviceMemory>(s.memory));
        if (s.buffer) vkDestroyBuffer(device, static_cast<VkBuffer>(s.buffer), nullptr);
        if (s.memory) vkFreeMemory(device, static_cast<VkDeviceMemory>(s.memory), nullptr);
        s = Slot{};
    }
    if (m_dummyView) vkDestroyImageView(device, static_cast<VkImageView>(m_dummyView), nullptr);
    if (m_dummy) vkDestroyImage(device, static_cast<VkImage>(m_dummy), nullptr);
    if (m_dummyMem) vkFreeMemory(device, static_cast<VkDeviceMemory>(m_dummyMem), nullptr);
    if (m_sampler) vkDestroySampler(device, static_cast<VkSampler>(m_sampler), nullptr);
    m_dummyView = m_dummy = m_dummyMem = m_sampler = nullptr;
    m_dummyReady = false;
    for (void*& e : m_external) e = nullptr;
    if (m_pool) vkDestroyDescriptorPool(device, static_cast<VkDescriptorPool>(m_pool), nullptr);
    if (m_pipeline) vkDestroyPipeline(device, static_cast<VkPipeline>(m_pipeline), nullptr);
    if (m_layout) vkDestroyPipelineLayout(device, static_cast<VkPipelineLayout>(m_layout), nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(device, static_cast<VkDescriptorSetLayout>(m_setLayout), nullptr);
    m_pool = m_pipeline = m_layout = m_setLayout = nullptr;
    m_desc = UiVkDesc{};
}

bool UiVkRenderer::upload(u32 slotIndex, const GpuDrawData& d) {
    if (!valid() || slotIndex >= m_desc.framesInFlight) return false;
    if (d.cmds.size() > m_desc.maxCmds || d.tiles.size() > m_desc.maxTileWords ||
        d.images.size() > m_desc.maxImageWords || d.atlasSize > m_desc.maxAtlasSize)
        return false;
    Slot& s = m_slots[slotIndex];
    std::memcpy(s.mapped + m_offsets[0], d.cmds.data(), d.cmdBytes());
    std::memcpy(s.mapped + m_offsets[1], d.tiles.data(), d.tileBytes());
    if (d.atlas && d.atlasSize) std::memcpy(s.mapped + m_offsets[2], d.atlas, d.atlasBytes());
    std::memcpy(s.mapped + m_offsets[3], d.images.data(), d.imageBytes());
    s.push = d.push;
    return true;
}

void UiVkRenderer::setExternalImage(u32 ext, void* view) {
    if (ext < kMaxExternal) m_external[ext] = view;
}

void UiVkRenderer::record(void* commandBuffer, void* targetView, u32 slotIndex) {
    if (!valid() || slotIndex >= m_desc.framesInFlight) return;
    const Slot& s = m_slots[slotIndex];
    if (s.push.extent[0] == 0 || s.push.extent[1] == 0) return;
    const VkDevice device = static_cast<VkDevice>(m_desc.device);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    if (!m_dummyReady) {
        // the dummy is never read (no command references an unbound slot) but must be in the declared layout
        VkImageMemoryBarrier db{};
        db.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        db.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        db.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = static_cast<VkImage>(m_dummy);
        db.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &db);
        m_dummyReady = true;
    }
    VkDescriptorImageInfo ii[1 + kMaxExternal]{};
    VkWriteDescriptorSet w[1 + kMaxExternal]{};
    ii[0].imageView = static_cast<VkImageView>(targetView);
    ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    for (u32 e = 0; e < kMaxExternal; ++e) {
        ii[1 + e].sampler = static_cast<VkSampler>(m_sampler);
        ii[1 + e].imageView = static_cast<VkImageView>(m_external[e] ? m_external[e] : m_dummyView);
        ii[1 + e].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    for (u32 i = 0; i < 1 + kMaxExternal; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = static_cast<VkDescriptorSet>(s.set);
        w[i].dstBinding = i == 0 ? 0 : 4 + i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device, 1 + kMaxExternal, w, 0, nullptr);
    const VkDescriptorSet set = static_cast<VkDescriptorSet>(s.set);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(m_pipeline));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipelineLayout>(m_layout), 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(m_layout), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuPush),
                       &s.push);
    vkCmdDispatch(cmd, (s.push.extent[0] + kUiTile - 1) / kUiTile, (s.push.extent[1] + kUiTile - 1) / kUiTile, 1);
}

} // namespace fuse::ui::render
