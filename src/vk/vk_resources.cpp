#include "vk/vk_resources.h"

#include <algorithm>
#include <cstdio>

namespace {

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

uint32_t FindMemoryType(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

}  // namespace

namespace vk {

VkFormat ToVkFormat(rg::Format f) {
    switch (f) {
        case rg::Format::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case rg::Format::R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case rg::Format::D32_SFLOAT: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkImageAspectFlags AspectOf(rg::Format f) {
    return (f == rg::Format::D32_SFLOAT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageUsageFlags UsageOf(rg::Format f) {
    if (f == rg::Format::D32_SFLOAT) {
        return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
}

VkImageView CreateImageView(const Device& d, VkImage image, rg::Format f) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ToVkFormat(f);
    vi.subresourceRange = {AspectOf(f), 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(d.device, &vi, nullptr, &view) != VK_SUCCESS) {
        std::printf("[Resources] failed to create image view\n");
    }
    return view;
}

void TransientPool::Build(const rg::CompiledGraph& graph,
                          const std::vector<rg::Resource>& resources,
                          const Device& dev) {
    // 计划签名：所有槽内的资源描述。窗口尺寸变化会改变 HDR/深度图尺寸，
    // 签名随之变化，触发重建；否则复用上一帧的图像。
    std::string sig;
    for (const auto& slot : graph.slots) {
        for (const auto& h : slot.resources) {
            const auto& r = resources[h.index];
            sig += std::to_string(h.index) + ":" + std::to_string(r.desc.width) + "x" +
                   std::to_string(r.desc.height) + ":" + std::to_string(static_cast<int>(r.desc.format)) +
                   ";";
        }
    }
    if (sig == m_signature && !m_handles.empty()) {
        return;  // 计划未变，句柄索引与上一帧一致，直接复用
    }

    Destroy(dev);
    m_signature = sig;
    m_handles.assign(resources.size(), ImageHandle{});
    m_sizes.assign(resources.size(), VkExtent2D{});

    for (const auto& slot : graph.slots) {
        Slot s;

        // 1) 创建槽内所有图像，查询各自内存需求。
        //    槽大小 = 所有图像需求的最大值（别名复用：它们共享同一块内存）。
        std::vector<VkImage> images;
        std::vector<rg::ResourceHandle> slotHandles;
        VkDeviceSize blockSize = 0;
        VkDeviceSize maxAlignment = 1;
        uint32_t typeBits = 0;
        for (const auto& h : slot.resources) {
            const auto& r = resources[h.index];
            VkImageCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = ToVkFormat(r.desc.format);
            ci.extent = {r.desc.width, r.desc.height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = UsageOf(r.desc.format);
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ci.flags = VK_IMAGE_CREATE_ALIAS_BIT;  // 允许与其他图像共享同一块内存

            VkImage img = VK_NULL_HANDLE;
            if (vkCreateImage(dev.device, &ci, nullptr, &img) != VK_SUCCESS) {
                std::printf("[Resources] failed to create transient image '%s'\n", r.name.c_str());
                continue;
            }
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(dev.device, img, &req);
            blockSize = std::max(blockSize, req.size);
            maxAlignment = std::max(maxAlignment, req.alignment);
            typeBits |= req.memoryTypeBits;
            images.push_back(img);
            slotHandles.push_back(h);
        }
        blockSize = AlignUp(blockSize, maxAlignment);

        // 2) 为整个槽分配一块显存
        const uint32_t type = FindMemoryType(dev.physicalDevice, typeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = blockSize;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(dev.device, &ai, nullptr, &s.memory) != VK_SUCCESS) {
            std::printf("[Resources] failed to allocate memory slot (%llu bytes)\n",
                        static_cast<unsigned long long>(blockSize));
        }
        s.size = blockSize;

        // 3) 所有图像绑定到同一偏移（offset 0），真正共享同一块显存
        for (size_t i = 0; i < images.size(); ++i) {
            vkBindImageMemory(dev.device, images[i], s.memory, 0);
            const auto& r = resources[slotHandles[i].index];
            m_handles[slotHandles[i].index] = {images[i], CreateImageView(dev, images[i], r.desc.format)};
            m_sizes[slotHandles[i].index] = {r.desc.width, r.desc.height};
        }
        m_slots.push_back(std::move(s));
        std::printf("[Pool] slot #%zu: %llu bytes, %zu resources aliased\n",
                    m_slots.size() - 1, static_cast<unsigned long long>(s.size), images.size());
    }
}

void TransientPool::Destroy(const Device& dev) {
    for (const auto& h : m_handles) {
        if (h.view) vkDestroyImageView(dev.device, h.view, nullptr);
        if (h.image) vkDestroyImage(dev.device, h.image, nullptr);
    }
    m_handles.clear();
    m_sizes.clear();
    for (const auto& s : m_slots) {
        if (s.memory) vkFreeMemory(dev.device, s.memory, nullptr);
    }
    m_slots.clear();
    m_signature.clear();
}

bool CreateBuffer(const Device& d, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d.device, &bi, nullptr, &out.buffer) != VK_SUCCESS) {
        std::printf("[Resources] failed to create buffer\n");
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d.device, out.buffer, &req);
    const uint32_t type = FindMemoryType(d.physicalDevice, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(d.device, &ai, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindBufferMemory(d.device, out.buffer, out.memory, 0) != VK_SUCCESS ||
        vkMapMemory(d.device, out.memory, 0, size, 0, &out.mapped) != VK_SUCCESS) {
        std::printf("[Resources] failed to allocate/map buffer memory\n");
        return false;
    }
    out.size = size;
    return true;
}

void Buffer::Destroy(const Device& d) {
    if (mapped) vkUnmapMemory(d.device, memory);
    if (buffer) vkDestroyBuffer(d.device, buffer, nullptr);
    if (memory) vkFreeMemory(d.device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    mapped = nullptr;
    size = 0;
}

}  // namespace vk
