// Nintendo Land Recomp - TV-Bild fuer das Online-Streaming von der Grafikkarte holen.
// Wird in CemuCafe mitgebaut (Mitglied von VulkanRenderer). Arbeitet ohne Warten: das Bild wird
// per Blit auf Streaming-Groesse gebracht und in einen Puffer kopiert; abgeholt wird es ein paar
// Bilder spaeter, wenn die Grafikkarte damit fertig ist.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VKRMemoryManager.h"
#include "Cafe/HW/Latte/Core/Latte.h"

#include <atomic>
#include <chrono>

// Schnittstelle zum Frontend
std::atomic<bool> g_nl_stream_active{false};
std::atomic<int> g_nl_stream_w{1280}, g_nl_stream_h{720}, g_nl_stream_fps{60};
void (*g_nl_stream_frame)(const uint8* rgba, int w, int h, int stride, bool srgb) = nullptr;
void (*g_nl_stream_audio)(const sint16* samples, int frames, int channels) = nullptr;   // aus ax_out.cpp

namespace {
struct NLCapSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMem = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMem = VK_NULL_HANDLE;
    void* ptr = nullptr;
    uint64 cmd = 0;
    uint64 order = 0;
    bool pending = false;
    bool srgb = false;
};
NLCapSlot s_cap[3];
int s_cap_w = 0, s_cap_h = 0;
uint64 s_cap_counter = 0;
std::chrono::steady_clock::time_point s_cap_last{};
}  // namespace

void VulkanRenderer::NL_StreamCapture(LatteTextureView* texView)
{
    // 1) Fertige Kopien abliefern, aelteste zuerst
    for (;;)
    {
        NLCapSlot* best = nullptr;
        for (auto& s : s_cap)
            if (s.pending && HasCommandBufferFinished(s.cmd) && (!best || s.order < best->order))
                best = &s;
        if (!best)
            break;
        best->pending = false;
        if (g_nl_stream_frame)
            g_nl_stream_frame((const uint8*)best->ptr, s_cap_w, s_cap_h, s_cap_w * 4, best->srgb);
    }
    if (!g_nl_stream_active || !texView)
        return;

    // 2) Bildrate begrenzen
    const auto now = std::chrono::steady_clock::now();
    const int fps = std::max(10, g_nl_stream_fps.load());
    if (now - s_cap_last < std::chrono::microseconds(1000000 / fps - 1500))
        return;

    // 3) Puffer anlegen / bei neuer Groesse neu anlegen
    const int w = g_nl_stream_w.load() & ~1, h = g_nl_stream_h.load() & ~1;
    if (w < 64 || h < 64)
        return;
    if (w != s_cap_w || h != s_cap_h)
    {
        for (auto& s : s_cap)
        {
            if (s.pending)
                WaitCommandBufferFinished(s.cmd);
            if (s.ptr) vkUnmapMemory(m_logicalDevice, s.bufferMem);
            if (s.buffer) vkDestroyBuffer(m_logicalDevice, s.buffer, nullptr);
            if (s.bufferMem) vkFreeMemory(m_logicalDevice, s.bufferMem, nullptr);
            if (s.image) vkDestroyImage(m_logicalDevice, s.image, nullptr);
            if (s.imageMem) vkFreeMemory(m_logicalDevice, s.imageMem, nullptr);
            s = NLCapSlot{};
        }
        s_cap_w = s_cap_h = 0;
        for (auto& s : s_cap)
        {
            VkImageCreateInfo ii{};
            ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent = {(uint32)w, (uint32)h, 1};
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_logicalDevice, &ii, nullptr, &s.image) != VK_SUCCESS)
                return;
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(m_logicalDevice, s.image, &req);
            uint32 memIndex = 0;
            if (!memoryManager->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memIndex))
                return;
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memIndex;
            if (vkAllocateMemory(m_logicalDevice, &ai, nullptr, &s.imageMem) != VK_SUCCESS)
                return;
            vkBindImageMemory(m_logicalDevice, s.image, s.imageMem, 0);
            const VkDeviceSize size = (VkDeviceSize)w * h * 4;
            if (!memoryManager->CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                             s.buffer, s.bufferMem) &&
                !memoryManager->CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                             s.buffer, s.bufferMem))
                return;
            vkMapMemory(m_logicalDevice, s.bufferMem, 0, VK_WHOLE_SIZE, 0, &s.ptr);
        }
        s_cap_w = w;
        s_cap_h = h;
    }

    // 4) freien Platz suchen (sonst dieses Bild auslassen)
    NLCapSlot* slot = nullptr;
    for (auto& s : s_cap)
        if (!s.pending) { slot = &s; break; }
    if (!slot)
        return;

    auto texViewVk = (LatteTextureViewVk*)texView;
    if (texViewVk->firstMip != 0)
        return;
    LatteTextureVk* baseTex = texViewVk->GetBaseImage();
    int sw = 0, sh = 0;
    baseTex->GetEffectiveSize(sw, sh, 0);
    if (sw <= 0 || sh <= 0)
        return;
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, baseTex->GetFormat(), &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            return;
    }
    s_cap_last = now;

    draw_endRenderPass();
    baseTex->GetImageObj()->flagForCurrentCommandBuffer();

    VkImageSubresourceRange dstRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier_image<TRANSFER_READ, TRANSFER_WRITE>(slot->image, dstRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceLayers srcLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32)texViewVk->firstSlice, 1};
    barrier_image<IMAGE_WRITE | TRANSFER_WRITE, SYNC_OP::TRANSFER_READ>(baseTex, srcLayers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImageBlit blit{};
    blit.srcSubresource = srcLayers;
    blit.srcOffsets[1] = {sw, sh, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {w, h, 1};
    vkCmdBlitImage(m_state.currentCommandBuffer, baseTex->GetImageObj()->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    VkImageSubresourceLayers backLayers{baseTex->GetImageAspect(), 0, (uint32)texViewVk->firstSlice, 1};
    barrier_image<TRANSFER_READ, TRANSFER_WRITE | IMAGE_WRITE>(baseTex, backLayers, baseTex->GetDefaultLayout());
    barrier_image<TRANSFER_WRITE, TRANSFER_READ>(slot->image, dstRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.bufferRowLength = (uint32)w;
    region.bufferImageHeight = (uint32)h;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32)w, (uint32)h, 1};
    vkCmdCopyImageToBuffer(m_state.currentCommandBuffer, slot->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->buffer, 1, &region);

    // Sichtbarkeit fuer die CPU
    VkBufferMemoryBarrier bb{};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = slot->buffer;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(m_state.currentCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);

    slot->cmd = GetCurrentCommandBufferId();
    slot->order = ++s_cap_counter;
    slot->srgb = LatteGPUState.tvBufferUsesSRGB;
    slot->pending = true;
}
