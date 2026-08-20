#pragma once

#include "demo/scene.h"
#include "rg/render_graph.h"
#include "vk/vk_backend.h"
#include "vk/vk_pipeline.h"
#include "vk/vk_resources.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace demo {

// Demo 上下文：所有 Pass 共享的持久状态（管线、几何、UBO、后端指针）。
struct VKTools {
    vk::Device* device = nullptr;
    vk::PipelineBundle* pipelines = nullptr;
    vk::Backend* backend = nullptr;
    vk::TransientPool* pool = nullptr;

    vk::Buffer vertexBuffer;
    vk::Buffer indexBuffer;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<glm::mat4> models;
    uint32_t indexCount = 0;
    uint32_t cubeIndexCount = 0;    // 立方体索引数（按模型逐个绘制）
    uint32_t groundIndexCount = 0;  // 地面索引数（单位矩阵绘制一次）

    vk::Buffer frameUbo[vk::kFramesInFlight];   // 场景 UBO（160B）
    vk::Buffer shadowUbo[vk::kFramesInFlight];  // 阴影 UBO（64B）
    FrameData frame{};
};

// 每个 Pass 的类型化数据：Setup 填充句柄，Execute 消费它们。
struct ShadowPassData {
    rg::ResourceHandle shadowMap;
};
struct ScenePassData {
    rg::ResourceHandle hdrColor;
    rg::ResourceHandle sceneDepth;
    rg::ResourceHandle shadowMap;
};
struct BlurPassData {
    rg::ResourceHandle hdrColor;
    rg::ResourceHandle blurBuffer;
};
struct PresentPassData {
    rg::ResourceHandle hdrColor;
    rg::ResourceHandle blurBuffer;
    rg::ResourceHandle swapImage;
};

// 一帧构建完成后暴露给 main 的资源句柄（用于描述符更新）。
struct FrameGraphResourceHandle {
    rg::ResourceHandle swapImage;
    rg::ResourceHandle shadowMap;
    rg::ResourceHandle hdrColor;
    rg::ResourceHandle sceneDepth;
    rg::ResourceHandle blurBuffer;
};

// 启动时创建几何缓冲与 UBO。
inline bool CreateDemoResources(VKTools& vkTools) {
    BuildGeometry(vkTools.vertices, vkTools.indices, vkTools.models);
    vkTools.indexCount = static_cast<uint32_t>(vkTools.indices.size());
    vkTools.cubeIndexCount = vkTools.indexCount - 6;  // 立方体 36 + 地面 6
    vkTools.groundIndexCount = 6;

    if (!vk::CreateBuffer(*vkTools.device, vkTools.vertices.size() * sizeof(Vertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vkTools.vertexBuffer)) {
        return false;
    }
    std::memcpy(vkTools.vertexBuffer.mapped, vkTools.vertices.data(),
                vkTools.vertices.size() * sizeof(Vertex));

    if (!vk::CreateBuffer(*vkTools.device, vkTools.indices.size() * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, vkTools.indexBuffer)) {
        return false;
    }
    std::memcpy(vkTools.indexBuffer.mapped, vkTools.indices.data(),
                vkTools.indices.size() * sizeof(uint32_t));

    for (uint32_t f = 0; f < vk::kFramesInFlight; ++f) {
        if (!vk::CreateBuffer(*vkTools.device, sizeof(FrameData),
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vkTools.frameUbo[f]) ||
            !vk::CreateBuffer(*vkTools.device, sizeof(glm::mat4),
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vkTools.shadowUbo[f])) {
            return false;
        }
    }
    return true;
}

inline void DestroyDemoResources(VKTools& demo) {
    for (uint32_t f = 0; f < vk::kFramesInFlight; ++f) {
        demo.frameUbo[f].Destroy(*demo.device);
        demo.shadowUbo[f].Destroy(*demo.device);
    }
    demo.indexBuffer.Destroy(*demo.device);
    demo.vertexBuffer.Destroy(*demo.device);
}

// 每帧构建图：导入交换链 -> 添加 4 个 Pass。
// 关键语义：资源在"第一个使用它的 Pass"里创建，后续 Pass 复用同一个句柄。
inline FrameGraphResourceHandle BuildGraph(rg::RenderGraph& graph, VKTools& demo) {
    FrameGraphResourceHandle handle;
    const uint32_t w = demo.backend->extent().width;
    const uint32_t h = demo.backend->extent().height;

    // 外部资源：当前交换链图像
    handle.swapImage = graph.Import({w, h, rg::Format::R8G8B8A8_UNORM}, "Swapchain");
    demo.backend->RegisterExternalImage(handle.swapImage.index, demo.backend->CurrentSwapImage(),
                                        demo.backend->CurrentSwapView());

    // ---- Pass 1: Shadow ----
    graph.AddPass(
        "Shadow",
        [&](rg::PassBuilder& b, ShadowPassData& d) {
            d.shadowMap = b.CreateTexture({2048, 2048, rg::Format::D32_SFLOAT}, "ShadowMap");
            handle.shadowMap = d.shadowMap;
            b.Write(d.shadowMap);
        },
        [&demo](const ShadowPassData& d, vk::ExecCtx& ctx) {
            ctx.BeginRendering({}, d.shadowMap);
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, demo.pipelines->shadow);
            ctx.SetViewport(ctx.SizeOf(d.shadowMap));
            const uint32_t f = ctx.frameIndex;
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    demo.pipelines->shadowLayout, 0, 1,
                                    &demo.pipelines->shadowSets[f], 0, nullptr);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &demo.vertexBuffer.buffer, &offset);
            vkCmdBindIndexBuffer(ctx.cmd, demo.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            // 立方体：每个模型绘制一次
            for (const auto& model : demo.models) {
                vkCmdPushConstants(ctx.cmd, demo.pipelines->shadowLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);
                vkCmdDrawIndexed(ctx.cmd, demo.cubeIndexCount, 1, 0, 0, 0);
            }
            // 地面：单位矩阵绘制一次（顶点已是世界空间）
            const glm::mat4 identity(1.0f);
            vkCmdPushConstants(ctx.cmd, demo.pipelines->shadowLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &identity);
            vkCmdDrawIndexed(ctx.cmd, demo.groundIndexCount, 1, demo.cubeIndexCount, 0, 0);
            ctx.EndRendering();
        });

    // ---- Pass 2: Scene ----
    graph.AddPass(
        "Scene",
        [&](rg::PassBuilder& b, ScenePassData& d) {
            d.hdrColor = b.CreateTexture({w, h, rg::Format::R16G16B16A16_SFLOAT}, "HDRColor");
            handle.hdrColor = d.hdrColor;
            d.sceneDepth = b.CreateTexture({w, h, rg::Format::D32_SFLOAT}, "SceneDepth");
            handle.sceneDepth = d.sceneDepth;
            d.shadowMap = handle.shadowMap;  // 复用 Shadow Pass 创建的句柄
            b.Read(d.shadowMap);
            b.Write(d.hdrColor);
            b.Write(d.sceneDepth);
        },
        [&demo](const ScenePassData& d, vk::ExecCtx& ctx) {
            ctx.BeginRendering(d.hdrColor, d.sceneDepth);
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, demo.pipelines->scene);
            ctx.SetViewport(ctx.SizeOf(d.hdrColor));
            const uint32_t f = ctx.frameIndex;
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    demo.pipelines->sceneLayout, 0, 1,
                                    &demo.pipelines->sceneSets[f], 0, nullptr);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &demo.vertexBuffer.buffer, &offset);
            vkCmdBindIndexBuffer(ctx.cmd, demo.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            // 立方体：每个模型绘制一次
            for (const auto& model : demo.models) {
                vkCmdPushConstants(ctx.cmd, demo.pipelines->sceneLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);
                vkCmdDrawIndexed(ctx.cmd, demo.cubeIndexCount, 1, 0, 0, 0);
            }
            // 地面：单位矩阵绘制一次
            const glm::mat4 identity(1.0f);
            vkCmdPushConstants(ctx.cmd, demo.pipelines->sceneLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &identity);
            vkCmdDrawIndexed(ctx.cmd, demo.groundIndexCount, 1, demo.cubeIndexCount, 0, 0);
            ctx.EndRendering();
        });

    // ---- Pass 3: Blur ----
    graph.AddPass(
        "Blur",
        [&](rg::PassBuilder& b, BlurPassData& d) {
            d.hdrColor = handle.hdrColor;  // 复用 Scene Pass 创建的句柄
            d.blurBuffer = b.CreateTexture({std::max(1u, w / 2), std::max(1u, h / 2),
                                            rg::Format::R16G16B16A16_SFLOAT},
                                           "BlurBuffer");
            handle.blurBuffer = d.blurBuffer;
            b.Read(d.hdrColor);
            b.Write(d.blurBuffer);
        },
        [&demo](const BlurPassData& d, vk::ExecCtx& ctx) {
            ctx.BeginRendering(d.blurBuffer, {});
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, demo.pipelines->blur);
            ctx.SetViewport(ctx.SizeOf(d.blurBuffer));
            const uint32_t f = ctx.frameIndex;
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    demo.pipelines->blurLayout, 0, 1,
                                    &demo.pipelines->blurSets[f], 0, nullptr);
            const glm::vec2 texelSize = {1.0f / static_cast<float>(ctx.extent.width),
                                         1.0f / static_cast<float>(ctx.extent.height)};
            vkCmdPushConstants(ctx.cmd, demo.pipelines->blurLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(texelSize), &texelSize);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            ctx.EndRendering();
        });

    // ---- Pass 4: Present ----
    graph.AddPass(
        "Present",
        [&](rg::PassBuilder& b, PresentPassData& d) {
            d.hdrColor = handle.hdrColor;    // 复用句柄
            d.blurBuffer = handle.blurBuffer;
            d.swapImage = handle.swapImage;  // 复用导入的交换链图像
            b.Read(d.hdrColor);
            b.Read(d.blurBuffer);
            b.Write(d.swapImage);
        },
        [&demo](const PresentPassData& d, vk::ExecCtx& ctx) {
            ctx.BeginRendering(d.swapImage, {});
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, demo.pipelines->present);
            ctx.SetViewport(ctx.SizeOf(d.swapImage));
            const uint32_t f = ctx.frameIndex;
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    demo.pipelines->presentLayout, 0, 1,
                                    &demo.pipelines->presentSets[f], 0, nullptr);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            ctx.EndRendering();
        });

    return handle;
}

// 每帧更新描述符（UBO + 采样图像）。图像在 TransientPool.Build 之后才存在，
// 所以放在 Execute 阶段、录制命令之前调用。
inline void UpdateFrameDescriptors(VKTools& vkTools, const FrameGraphResourceHandle& g) {
    const uint32_t f = vkTools.backend->ctx().frameIndex;

    std::memcpy(vkTools.frameUbo[f].mapped, &vkTools.frame, sizeof(FrameData));
    std::memcpy(vkTools.shadowUbo[f].mapped, &vkTools.frame.lightVP, sizeof(glm::mat4));

    const VkDescriptorBufferInfo frameUboInfo{vkTools.frameUbo[f].buffer, 0, sizeof(FrameData)};
    const VkDescriptorBufferInfo shadowUboInfo{vkTools.shadowUbo[f].buffer, 0, sizeof(glm::mat4)};
    const VkDescriptorImageInfo shadowImg{
        vkTools.pipelines->shadowSampler, vkTools.pool->ViewOf(g.shadowMap),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo hdrImg{
        vkTools.pipelines->linearSampler, vkTools.pool->ViewOf(g.hdrColor),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo blurImg{
        vkTools.pipelines->linearSampler, vkTools.pool->ViewOf(g.blurBuffer),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[6]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, vkTools.pipelines->shadowSets[f],
                 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &shadowUboInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, vkTools.pipelines->sceneSets[f],
                 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &frameUboInfo, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, vkTools.pipelines->sceneSets[f],
                 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowImg, nullptr, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, vkTools.pipelines->blurSets[f],
                 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hdrImg, nullptr, nullptr};
    writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, vkTools.pipelines->presentSets[f],
                 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hdrImg, nullptr, nullptr};
    writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, vkTools.pipelines->presentSets[f],
                 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurImg, nullptr, nullptr};
    vkUpdateDescriptorSets(vkTools.device->device, 6, writes, 0, nullptr);
}

}  // namespace demo
