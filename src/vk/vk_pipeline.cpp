#include "vk/vk_pipeline.h"

#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

// 按可执行文件所在目录解析相对路径（运行时 shaders 复制到 exe 旁）。
std::string ExeRelativePath(const std::string& relative) {
    char buffer[MAX_PATH];
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::string path(buffer);
    const size_t pos = path.find_last_of("\\/");
    return path.substr(0, pos) + "\\" + relative;
}

std::vector<uint32_t> LoadSpv(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::printf("[Pipeline] cannot open shader: %s\n", path.c_str());
        return {};
    }
    const std::streamsize size = file.tellg();
    std::vector<uint32_t> code(static_cast<size_t>(size) / sizeof(uint32_t));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(code.data()), size);
    return code;
}

VkShaderModule CreateModule(const vk::Device& d, const std::string& path) {
    const std::vector<uint32_t> code = LoadSpv(path);
    if (code.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(d.device, &ci, nullptr, &module);
    return module;
}

VkDescriptorSetLayout CreateSetLayout(
    const vk::Device& d,
    const std::vector<std::pair<VkDescriptorType, VkShaderStageFlags>>& bindings) {
    std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
    layoutBindings.reserve(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        layoutBindings.push_back(
            {static_cast<uint32_t>(i), bindings[i].first, 1, bindings[i].second, nullptr});
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = static_cast<uint32_t>(layoutBindings.size());
    ci.pBindings = layoutBindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(d.device, &ci, nullptr, &layout);
    return layout;
}

VkPipelineLayout CreateLayout(const vk::Device& d, VkDescriptorSetLayout setLayout,
                              const VkPushConstantRange* pushRanges, uint32_t pushCount) {
    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &setLayout;
    ci.pushConstantRangeCount = pushCount;
    ci.pPushConstantRanges = pushRanges;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(d.device, &ci, nullptr, &layout);
    return layout;
}

struct PipelineSpec {
    const char* vertPath;
    const char* fragPath;
    VkPipelineLayout layout;
    VkFormat colorFormat;  // VK_FORMAT_UNDEFINED = 无颜色附件
    VkFormat depthFormat;  // VK_FORMAT_UNDEFINED = 无深度附件
    bool depthBias = false;
    const std::vector<VkVertexInputBindingDescription>* bindings = nullptr;
    const std::vector<VkVertexInputAttributeDescription>* attributes = nullptr;
};

VkPipeline CreateGraphicsPipeline(const vk::Device& d, const std::string& shaderDir,
                                  const PipelineSpec& spec) {
    const VkShaderModule vertModule =
        CreateModule(d, shaderDir + "\\" + spec.vertPath + ".spv");
    VkShaderModule fragModule = VK_NULL_HANDLE;
    if (spec.fragPath) {
        fragModule = CreateModule(d, shaderDir + "\\" + spec.fragPath + ".spv");
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    uint32_t stageCount = 1;
    if (fragModule) {
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";
        stageCount = 2;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (spec.bindings && !spec.bindings->empty()) {
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(spec.bindings->size());
        vertexInput.pVertexBindingDescriptions = spec.bindings->data();
    }
    if (spec.attributes && !spec.attributes->empty()) {
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(spec.attributes->size());
        vertexInput.pVertexAttributeDescriptions = spec.attributes->data();
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    // 投影矩阵做了 Y 翻转以适配 Vulkan NDC（见 demo/scene.h），
    // 几何在帧缓冲中的绕序随之反转，因此正面 = 顺时针。
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    if (spec.depthBias) {
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 1.25f;
        raster.depthBiasSlopeFactor = 1.75f;
    }

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (spec.depthFormat != VK_FORMAT_UNDEFINED) {
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    }

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    if (spec.colorFormat != VK_FORMAT_UNDEFINED) {
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;
    }

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat colorFormats[1] = {spec.colorFormat};
    if (spec.colorFormat != VK_FORMAT_UNDEFINED) {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = colorFormats;
    }
    rendering.depthAttachmentFormat = spec.depthFormat;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rendering;
    ci.stageCount = stageCount;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState = &viewportState;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depthStencil;
    ci.pColorBlendState = &colorBlend;
    ci.pDynamicState = &dynamicState;
    ci.layout = spec.layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        std::printf("[Pipeline] failed to create pipeline (stages: %s, %s)\n",
                    spec.vertPath, spec.fragPath ? spec.fragPath : "(none)");
    }
    vkDestroyShaderModule(d.device, vertModule, nullptr);
    if (fragModule) vkDestroyShaderModule(d.device, fragModule, nullptr);
    return pipeline;
}

}  // namespace

namespace vk {

bool CreatePipelines(const Device& d, const char* shaderDir, VkFormat swapFormat,
                     PipelineBundle& out) {
    const std::string dir = ExeRelativePath(shaderDir);

    // ---- 描述符集布局 ----
    out.shadowSetLayout = CreateSetLayout(
        d, {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT}});
    out.sceneSetLayout = CreateSetLayout(
        d, {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}});
    out.blurSetLayout = CreateSetLayout(
        d, {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}});
    out.presentSetLayout = CreateSetLayout(
        d, {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}});

    // ---- 管线布局（含 push constant） ----
    // VkPushConstantRange 字段顺序：stageFlags, offset, size
    const VkPushConstantRange pushMat4{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 16};
    const VkPushConstantRange pushVec2{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 2};
    out.shadowLayout = CreateLayout(d, out.shadowSetLayout, &pushMat4, 1);
    out.sceneLayout = CreateLayout(d, out.sceneSetLayout, &pushMat4, 1);
    out.blurLayout = CreateLayout(d, out.blurSetLayout, &pushVec2, 1);
    out.presentLayout = CreateLayout(d, out.presentSetLayout, nullptr, 0);

    // ---- 采样器 ----
    VkSamplerCreateInfo linearInfo{};
    linearInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    linearInfo.magFilter = VK_FILTER_LINEAR;
    linearInfo.minFilter = VK_FILTER_LINEAR;
    linearInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    linearInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    linearInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    linearInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(d.device, &linearInfo, nullptr, &out.linearSampler);

    VkSamplerCreateInfo shadowInfo = linearInfo;
    shadowInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    shadowInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    shadowInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    shadowInfo.compareEnable = VK_TRUE;
    shadowInfo.compareOp = VK_COMPARE_OP_LESS;
    vkCreateSampler(d.device, &shadowInfo, nullptr, &out.shadowSampler);

    // ---- 描述符池与集合（每帧一套） ----
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6},      // (shadow + scene) * 3 帧
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12},  // (scene1 + blur1 + present2) * 3 帧
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 12;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(d.device, &poolInfo, nullptr, &out.pool) != VK_SUCCESS) {
        std::printf("[Pipeline] failed to create descriptor pool\n");
        return false;
    }
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = out.pool;
    alloc.descriptorSetCount = 1;
    VkDescriptorSetLayout oneLayout = VK_NULL_HANDLE;
    for (uint32_t f = 0; f < kFramesInFlight; ++f) {
        oneLayout = out.shadowSetLayout;
        alloc.pSetLayouts = &oneLayout;
        vkAllocateDescriptorSets(d.device, &alloc, &out.shadowSets[f]);

        oneLayout = out.sceneSetLayout;
        alloc.pSetLayouts = &oneLayout;
        vkAllocateDescriptorSets(d.device, &alloc, &out.sceneSets[f]);

        oneLayout = out.blurSetLayout;
        alloc.pSetLayouts = &oneLayout;
        vkAllocateDescriptorSets(d.device, &alloc, &out.blurSets[f]);

        oneLayout = out.presentSetLayout;
        alloc.pSetLayouts = &oneLayout;
        vkAllocateDescriptorSets(d.device, &alloc, &out.presentSets[f]);
    }

    // ---- 顶点输入 ----
    // 注意：顶点缓冲是 pos+normal+color 交错布局（stride=36），
    // 阴影管线只读 position（location 0），但 stride 必须匹配真实布局，
    // 否则每 12 字节读一个"顶点"会把法线/颜色当成位置，阴影贴图变成乱码。
    const VkVertexInputBindingDescription shadowBinding{0, sizeof(float) * 9,
                                                       VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription shadowAttr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    const std::vector<VkVertexInputBindingDescription> shadowBindings = {shadowBinding};
    const std::vector<VkVertexInputAttributeDescription> shadowAttrs = {shadowAttr};

    const VkVertexInputBindingDescription sceneBinding{0, sizeof(float) * 9,
                                                       VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription sceneAttrs[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 6},
    };
    const std::vector<VkVertexInputBindingDescription> sceneBindings = {sceneBinding};
    const std::vector<VkVertexInputAttributeDescription> sceneAttrsVec(
        sceneAttrs, sceneAttrs + 3);

    // ---- 图形管线 ----
    const VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    out.shadow = CreateGraphicsPipeline(
        d, dir, {"shadow.vert", "shadow.frag", out.shadowLayout, VK_FORMAT_UNDEFINED,
                 kDepthFormat, /*depthBias=*/true, &shadowBindings, &shadowAttrs});
    out.scene = CreateGraphicsPipeline(
        d, dir, {"scene.vert", "scene.frag", out.sceneLayout, kHdrFormat, kDepthFormat,
                 /*depthBias=*/false, &sceneBindings, &sceneAttrsVec});
    out.blur = CreateGraphicsPipeline(
        d, dir, {"fullscreen.vert", "blur.frag", out.blurLayout, kHdrFormat,
                 VK_FORMAT_UNDEFINED, false, nullptr, nullptr});
    out.present = CreateGraphicsPipeline(
        d, dir, {"fullscreen.vert", "present.frag", out.presentLayout, swapFormat,
                 VK_FORMAT_UNDEFINED, false, nullptr, nullptr});

    return out.shadow && out.scene && out.blur && out.present;
}

void DestroyPipelines(const Device& d, PipelineBundle& out) {
    if (d.device == VK_NULL_HANDLE) return;
    vkDestroyPipeline(d.device, out.shadow, nullptr);
    vkDestroyPipeline(d.device, out.scene, nullptr);
    vkDestroyPipeline(d.device, out.blur, nullptr);
    vkDestroyPipeline(d.device, out.present, nullptr);
    vkDestroyPipelineLayout(d.device, out.shadowLayout, nullptr);
    vkDestroyPipelineLayout(d.device, out.sceneLayout, nullptr);
    vkDestroyPipelineLayout(d.device, out.blurLayout, nullptr);
    vkDestroyPipelineLayout(d.device, out.presentLayout, nullptr);
    vkDestroyDescriptorSetLayout(d.device, out.shadowSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(d.device, out.sceneSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(d.device, out.blurSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(d.device, out.presentSetLayout, nullptr);
    vkDestroyDescriptorPool(d.device, out.pool, nullptr);
    vkDestroySampler(d.device, out.linearSampler, nullptr);
    vkDestroySampler(d.device, out.shadowSampler, nullptr);
    out = PipelineBundle{};
}

}  // namespace vk
