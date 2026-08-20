#pragma once

#include "rg/compile.h"
#include "rg/render_graph.h"
#include "vk/vk_device.h"
#include "vk/vk_pipeline.h"
#include "vk/vk_resources.h"

#include <unordered_map>
#include <vector>

namespace vk {

// Execute 上下文：Pass 的 execute 回调通过它拿物理资源并录制命令。
// 图核心（rg）-> 后端（vk）的接口。
struct ExecCtx {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    const TransientPool* pool = nullptr;
    const std::unordered_map<uint32_t, VkImage>* externals = nullptr;
    const std::unordered_map<uint32_t, VkImageView>* externalViews = nullptr;
    VkExtent2D extent{};
    uint32_t frameIndex = 0;

    VkImageView ViewOf(rg::ResourceHandle h) const;
    VkExtent2D SizeOf(rg::ResourceHandle h) const;
    // 动态视口（管线以动态状态创建，避免重建管线）
    void SetViewport(VkExtent2D size) const;

    void BeginRendering(rg::ResourceHandle color, rg::ResourceHandle depth) const;
    void EndRendering() const;
};

// 后端（vulkan）：帧同步（2 帧在途）、屏障插入、Pass 分发、呈现。
class Backend {
public:
    explicit Backend(Device& device);
    ~Backend();

    // 等待 fence -> acquire 交换链图像 -> 开始命令缓冲
    bool BeginFrame();
    // 按拓扑序插入屏障并调用各 Pass 的执行回调
    void RecordFrame(const rg::CompiledGraph& graph, const std::vector<rg::Pass>& passes,
                     const std::vector<rg::Resource>& resources);
    // 提交命令缓冲 -> 呈现
    bool EndFrame();

    void RegisterExternalImage(uint32_t handleIndex, VkImage image, VkImageView view);
    void SetPool(TransientPool* pool) { m_pool = pool; }
    VkImage CurrentSwapImage() const { return m_currentSwapImage; }
    VkImageView CurrentSwapView() const { return m_currentSwapView; }

    // 按新图像数重建 per-image present 信号量。
    void OnSwapchainRecreated();

    VkExtent2D extent() const { return m_device.extent; }
    const ExecCtx& ctx() const { return m_ctx; }
    bool ShouldRecreateSwapchain() const { return m_resizeRequested; }
    void ClearResizeRequest() { m_resizeRequested = false; }

private:
    void CreatePresentSemaphores();

    void InsertTransition(const rg::Transition& t, const std::vector<rg::Resource>& resources);
    VkImage ImageOf(rg::ResourceHandle h) const;

    Device& m_device;
    TransientPool* m_pool = nullptr;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmds[kFramesInFlight];

    struct FrameSync {
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
    };
    FrameSync m_frames[kFramesInFlight];
    // present 信号量：每张交换链图像一个。图像被重新 acquire 前，其上一次
    // present 必然已完成，因此该信号量一定处于未 signal 状态，可安全复用。
    std::vector<VkSemaphore> m_render;
    uint32_t m_frameIndex = 0;
    uint32_t m_swapIndex = 0;
    bool m_resizeRequested = false;

    VkImage m_currentSwapImage = VK_NULL_HANDLE;
    VkImageView m_currentSwapView = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkImage> m_externals;
    std::unordered_map<uint32_t, VkImageView> m_externalViews;
    ExecCtx m_ctx;
};

}  // namespace vk
