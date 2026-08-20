#pragma once

#include "vk/vk_device.h"

namespace vk {

// 帧同步资源份数（fence / acquire 信号量 / 命令缓冲 / 描述符集 / UBO）：
// 与交换链图像数一致（3）。实际 2 帧在途——每帧开录前只等两帧前的 fence
// （见 Backend::BeginFrame）；present 信号量按图像索引单独管理，不在此列。
constexpr uint32_t kFramesInFlight = 3;

// 全部管线与描述符的集合：启动时一次创建，帧间复用。
struct PipelineBundle {
    VkDescriptorSetLayout shadowSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout blurSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout presentSetLayout = VK_NULL_HANDLE;

    VkPipelineLayout shadowLayout = VK_NULL_HANDLE;
    VkPipelineLayout sceneLayout = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout = VK_NULL_HANDLE;
    VkPipelineLayout presentLayout = VK_NULL_HANDLE;

    VkPipeline shadow = VK_NULL_HANDLE;
    VkPipeline scene = VK_NULL_HANDLE;
    VkPipeline blur = VK_NULL_HANDLE;
    VkPipeline present = VK_NULL_HANDLE;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet shadowSets[kFramesInFlight]{};
    VkDescriptorSet sceneSets[kFramesInFlight]{};
    VkDescriptorSet blurSets[kFramesInFlight]{};
    VkDescriptorSet presentSets[kFramesInFlight]{};

    VkSampler linearSampler = VK_NULL_HANDLE;
    VkSampler shadowSampler = VK_NULL_HANDLE;
};

bool CreatePipelines(const Device& d, const char* shaderDir, VkFormat swapFormat,
                     PipelineBundle& out);
void DestroyPipelines(const Device& d, PipelineBundle& out);

}  // namespace vk
