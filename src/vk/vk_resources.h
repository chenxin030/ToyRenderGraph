#pragma once

#include "rg/compile.h"
#include "vk/vk_device.h"

#include <string>
#include <vector>

namespace vk {

struct ImageHandle {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// 显存复用池（MemoryPool）：
// 编译结果把生命周期互不重叠的瞬时资源分到同一个槽（MemorySlot），
// 每个 slot 分配一块 VkDeviceMemory，槽内所有资源共享这一块内存。
// 比如 ShadowMap 与 BlurBuffer 共享同一块显存。
class TransientPool {
public:
    // 按编译计划重建（签名相同则跳过，避免每帧重建图像）。
    void Build(const rg::CompiledGraph& graph,
               const std::vector<rg::Resource>& resources,
               const Device& dev);

    void Destroy(const Device& dev);

    VkImage ImageOf(rg::ResourceHandle h) const { return m_handles[h.index].image; }
    VkImageView ViewOf(rg::ResourceHandle h) const { return m_handles[h.index].view; }
    VkExtent2D SizeOf(rg::ResourceHandle h) const { return m_sizes[h.index]; }

private:
    struct Slot {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    std::vector<Slot> m_slots;
    std::vector<ImageHandle> m_handles;  // 按资源句柄索引
    std::vector<VkExtent2D> m_sizes;     // 按资源句柄索引（附件尺寸，用于渲染区域）
    std::string m_signature;
};

// 小工具：host-visible 缓冲（UBO / 顶点 / 索引），映射后直接 memcpy。
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;

    void Destroy(const Device& d);
};

bool CreateBuffer(const Device& d, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);

// 抽象格式/状态 -> Vulkan 的映射
VkFormat ToVkFormat(rg::Format f);
VkImageAspectFlags AspectOf(rg::Format f);
VkImageUsageFlags UsageOf(rg::Format f);
VkImageView CreateImageView(const Device& d, VkImage image, rg::Format f);

}  // namespace vk
