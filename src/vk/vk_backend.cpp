#include "vk/vk_backend.h"

#include <cstdio>

namespace {

// 抽象状态 -> Vulkan 的 layout / stage / access 三元组。
struct LayoutStageAccess {
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

LayoutStageAccess InfoOf(rg::ResourceState s) {
    switch (s) {
        case rg::ResourceState::Undefined:
            return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
        case rg::ResourceState::ColorWrite:
            return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case rg::ResourceState::DepthWrite:
            return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case rg::ResourceState::ShaderRead:
            return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT};
        case rg::ResourceState::Present:
            return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0};
    }
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
}

}  // namespace

namespace vk {

VkImageView ExecCtx::ViewOf(rg::ResourceHandle h) const {
    if (auto it = externalViews->find(h.index); it != externalViews->end()) {
        return it->second;
    }
    return pool->ViewOf(h);
}

VkExtent2D ExecCtx::SizeOf(rg::ResourceHandle h) const {
    if (externalViews->find(h.index) != externalViews->end()) {
        return extent;  // 外部资源（交换链）尺寸 = 窗口尺寸
    }
    return pool->SizeOf(h);
}

void ExecCtx::SetViewport(VkExtent2D size) const {
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(size.width),
                              static_cast<float>(size.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, size};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void ExecCtx::BeginRendering(rg::ResourceHandle color, rg::ResourceHandle depth) const {
    VkRenderingAttachmentInfo colorAtt{};
    VkRenderingAttachmentInfo depthAtt{};

    // 注意：只在句柄有效时才取视图，否则 ViewOf 会越界访问资源表
    if (color.valid()) {
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = ViewOf(color);
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{0.1f, 0.1f, 0.15f, 1.0f}};
    }
    if (depth.valid()) {
        depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAtt.imageView = ViewOf(depth);
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAtt.clearValue.depthStencil = {1.0f, 0};
    }

    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    // 渲染区域必须与附件尺寸一致（Blur 等半分辨率附件不能用窗口尺寸）
    const VkExtent2D attachSize = color.valid() ? SizeOf(color) : SizeOf(depth);
    info.renderArea = {{0, 0}, attachSize};
    info.layerCount = 1;
    if (color.valid()) {
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &colorAtt;
    }
    if (depth.valid()) {
        info.pDepthAttachment = &depthAtt;
    }
    vkCmdBeginRendering(cmd, &info);
}

void ExecCtx::EndRendering() const {
    vkCmdEndRendering(cmd);
}

Backend::Backend(Device& device) : m_device(device) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_device.graphicsFamily;
    vkCreateCommandPool(m_device.device, &poolInfo, nullptr, &m_cmdPool);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = m_cmdPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kFramesInFlight;
    vkAllocateCommandBuffers(m_device.device, &alloc, m_cmds);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // 第一帧无需等待
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& f : m_frames) {
        vkCreateFence(m_device.device, &fenceInfo, nullptr, &f.fence);
        vkCreateSemaphore(m_device.device, &semInfo, nullptr, &f.acquire);
    }
    CreatePresentSemaphores();

    m_ctx.pool = nullptr;  // main 每帧 Build 后 SetPool
    m_ctx.externals = &m_externals;
    m_ctx.externalViews = &m_externalViews;
}

void Backend::CreatePresentSemaphores() {
    m_render.resize(m_device.swapImages.size());
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& sem : m_render) {
        vkCreateSemaphore(m_device.device, &semInfo, nullptr, &sem);
    }
}

void Backend::OnSwapchainRecreated() {
    // 调用前提：vk::RecreateSwapchain 内部已 vkDeviceWaitIdle，
    // 所有旧 present 已结束，可安全销毁旧信号量。
    for (const auto sem : m_render) {
        vkDestroySemaphore(m_device.device, sem, nullptr);
    }
    CreatePresentSemaphores();
}

Backend::~Backend() {
    vkDeviceWaitIdle(m_device.device);
    for (auto& f : m_frames) {
        vkDestroyFence(m_device.device, f.fence, nullptr);
        vkDestroySemaphore(m_device.device, f.acquire, nullptr);
    }
    for (const auto sem : m_render) {
        vkDestroySemaphore(m_device.device, sem, nullptr);
    }
    vkDestroyCommandPool(m_device.device, m_cmdPool, nullptr);
}

bool Backend::BeginFrame() {
    if (m_device.swapchain == VK_NULL_HANDLE) {
        return false;
    }

    // index + 1 == 2 帧前的帧
    const uint32_t prevFrame = (m_frameIndex + kFramesInFlight - 2) % kFramesInFlight;
    vkWaitForFences(m_device.device, 1, &m_frames[prevFrame].fence, VK_TRUE, UINT64_MAX);

    // 有限超时：窗口不可见/被遮挡时 acquire 可能长时间无图像可用，
    // 不无限阻塞主循环。
    const VkResult acquire = vkAcquireNextImageKHR(
        m_device.device, m_device.swapchain, 114514, m_frames[m_frameIndex].acquire,
        VK_NULL_HANDLE, &m_swapIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        m_resizeRequested = true;
        return false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        // 超时等异常：跳过本帧；下一帧成功 acquire 后会重置命令缓冲再重试
        std::printf("[Backend] acquire returned %d, skipping frame\n", static_cast<int>(acquire));
        return false;
    }
    if (acquire == VK_SUBOPTIMAL_KHR) {
        m_resizeRequested = true;  // 仍可继续，下一帧重建
    }

    vkResetFences(m_device.device, 1, &m_frames[m_frameIndex].fence);
    vkResetCommandBuffer(m_cmds[m_frameIndex], 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmds[m_frameIndex], &begin);

    m_currentSwapImage = m_device.swapImages[m_swapIndex];
    m_currentSwapView = m_device.swapViews[m_swapIndex];

    m_ctx.cmd = m_cmds[m_frameIndex];
    m_ctx.extent = m_device.extent;
    m_ctx.frameIndex = m_frameIndex;
    return true;
}

void Backend::RecordFrame(const rg::CompiledGraph& graph,
                          const std::vector<rg::Pass>& passes,
                          const std::vector<rg::Resource>& resources) {
    m_ctx.pool = m_pool;
    for (const auto& cp : graph.passes) {
        for (const auto& t : cp.before) InsertTransition(t, resources);
        passes[cp.passIndex].execute(&m_ctx);
        for (const auto& t : cp.after) InsertTransition(t, resources);
    }
}

bool Backend::EndFrame() {
    vkEndCommandBuffer(m_cmds[m_frameIndex]);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &m_frames[m_frameIndex].acquire;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_cmds[m_frameIndex];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_render[m_swapIndex];
    if (vkQueueSubmit(m_device.queue, 1, &submit, m_frames[m_frameIndex].fence) != VK_SUCCESS) {
        std::printf("[Backend] queue submit failed\n");
        return false;
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_render[m_swapIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &m_device.swapchain;
    present.pImageIndices = &m_swapIndex;
    const VkResult result = vkQueuePresentKHR(m_device.queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_resizeRequested = true;
    }

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
    return true;
}

void Backend::RegisterExternalImage(uint32_t handleIndex, VkImage image, VkImageView view) {
    m_externals[handleIndex] = image;
    m_externalViews[handleIndex] = view;
}

VkImage Backend::ImageOf(rg::ResourceHandle h) const {
    if (auto it = m_externals.find(h.index); it != m_externals.end()) {
        return it->second;
    }
    return m_pool->ImageOf(h);
}

void Backend::InsertTransition(const rg::Transition& t,
                               const std::vector<rg::Resource>& resources) {
    const auto& res = resources[t.handle.index];
    const LayoutStageAccess dst = InfoOf(t.newState);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.oldLayout = InfoOf(t.oldState).layout;
    barrier.newLayout = dst.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = ImageOf(t.handle);
    barrier.subresourceRange = {AspectOf(res.desc.format), 0, 1, 0, 1};

    if (t.aliasHandoff) {
        // 别名交接：源端是前一资源对该内存的最后一次访问
        const LayoutStageAccess src = InfoOf(t.aliasSrcState);
        barrier.srcStageMask = src.stage;
        barrier.srcAccessMask = src.access;
    } else {
        const LayoutStageAccess src = InfoOf(t.oldState);
        barrier.srcStageMask = src.stage;
        barrier.srcAccessMask = src.access;
    }
    barrier.dstStageMask = dst.stage;
    barrier.dstAccessMask = dst.access;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    std::printf("[Barrier] '%s': %d -> %d%s\n", res.name.c_str(),
                static_cast<int>(t.oldState), static_cast<int>(t.newState),
                t.aliasHandoff ? " (alias handoff)" : "");
    vkCmdPipelineBarrier2(m_cmds[m_frameIndex], &dep);
}

}  // namespace vk
