#include "demo/passes.h"
#include "rg/compile.h"
#include "rg/render_graph.h"
#include "vk/vk_backend.h"
#include "vk/vk_device.h"
#include "vk/vk_pipeline.h"
#include "vk/vk_resources.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <memory>

namespace {

bool g_resized = false;

void FramebufferResizeCallback(GLFWwindow*, int, int) {
    g_resized = true;
}

// 编译结果日志：Pass 顺序、死枝剔除、显存复用计划。只在首帧打印一次。
void PrintCompileLog(const rg::CompiledGraph& compiled,
                     const std::vector<rg::Pass>& passes,
                     const std::vector<rg::Resource>& resources) {
    std::printf("\n========== Compile Result ==========\n");
    std::printf("Pass order: ");
    for (size_t i = 0; i < compiled.passes.size(); ++i) {
        std::printf("'%s'", passes[compiled.passes[i].passIndex].name.c_str());
        if (i + 1 < compiled.passes.size()) std::printf(" -> ");
    }
    std::printf("\n");
    if (!compiled.stripped.empty()) {
        std::printf("Dead-stripped: ");
        for (const uint32_t p : compiled.stripped) {
            std::printf("'%s' ", passes[p].name.c_str());
        }
        std::printf("\n");
    }
    std::printf("Memory slots (%zu):\n", compiled.slots.size());
    for (size_t s = 0; s < compiled.slots.size(); ++s) {
        std::printf("  slot #%zu: ", s);
        for (const auto& h : compiled.slots[s].resources) {
            const auto& r = resources[h.index];
            std::printf("'%s'(%ux%u) ", r.name.c_str(), r.desc.width, r.desc.height);
        }
        std::printf("\n");
    }
    std::printf("====================================\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // 日志实时可见（重定向/VS 控制台都适用）

    if (!glfwInit()) {
        std::printf("failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "ToyRenderGraph", nullptr, nullptr);
    if (!window) {
        std::printf("failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwSetFramebufferSizeCallback(window, FramebufferResizeCallback);

    vk::Device device;
    if (!vk::CreateInstance(device)) return 1;
    if (!vk::CreateSurface(device, window)) return 1;
    if (!vk::PickPhysicalDevice(device)) return 1;
    if (!vk::CreateDevice(device)) return 1;
    if (!vk::CreateSwapchain(device, window)) return 1;

    vk::PipelineBundle pipelines;
    if (!vk::CreatePipelines(device, "shaders", device.swapFormat, pipelines)) return 1;
    std::printf("[init] pipelines ok\n");

    vk::TransientPool pool;
    auto backend = std::make_unique<vk::Backend>(device);
    backend->SetPool(&pool);
    std::printf("[init] backend ok\n");

    demo::VKTools vkTools;
    vkTools.device = &device;
    vkTools.pipelines = &pipelines;
    vkTools.backend = backend.get();
    vkTools.pool = &pool;
    if (!demo::CreateDemoResources(vkTools)) return 1;
    std::printf("[init] demo resources ok\n");

    auto start = std::chrono::steady_clock::now();
    auto lastFrame = start;
    double fpsAccum = 0.0;
    uint64_t fpsFrames = 0;
    bool compileLogged = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (g_resized || backend->ShouldRecreateSwapchain()) {
            int fbWidth = 0, fbHeight = 0;
            glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
            if (fbWidth > 0 && fbHeight > 0) {
                if (vk::RecreateSwapchain(device, window)) {
                    backend->OnSwapchainRecreated();
                    g_resized = false;
                    backend->ClearResizeRequest();
                } else {
                    std::printf("[loop] swapchain recreate failed, will retry\n");
                }
            }
        }

        // acquire 失败（如窗口最小化）时跳过本帧
        if (!backend->BeginFrame()) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;
        const float time = std::chrono::duration<float>(now - start).count();
        demo::UpdateFrameData(vkTools.frame, time,
                              static_cast<float>(device.extent.width) / device.extent.height);

        // Phase 1: Setup
        rg::RenderGraph graph;
        const demo::FrameGraphResourceHandle fgResourcehandles = demo::BuildGraph(graph, vkTools);

        // Phase 2: Compile
        const rg::CompiledGraph compiled =
            rg::Compile(graph.passes(), graph.resources(), {fgResourcehandles.swapImage});
        if (!compileLogged) {
            PrintCompileLog(compiled, graph.passes(), graph.resources());
            compileLogged = true;
        }

        // Phase 3: Execute
        pool.Build(compiled, graph.resources(), device);
        demo::UpdateFrameDescriptors(vkTools, fgResourcehandles);
        backend->RecordFrame(compiled, graph.passes(), graph.resources());
        backend->EndFrame();

        // 标题栏 FPS
        fpsAccum += dt;
        ++fpsFrames;
        if (fpsAccum >= 0.5) {
            char title[128];
            std::snprintf(title, sizeof(title), "ToyRenderGraph - %.0f FPS",
                          fpsFrames / fpsAccum);
            glfwSetWindowTitle(window, title);
            fpsAccum = 0.0;
            fpsFrames = 0;
        }
    }

    vkDeviceWaitIdle(device.device);
    demo::DestroyDemoResources(vkTools);
    backend.reset();  // 必须在 device 销毁前
    pool.Destroy(device);
    vk::DestroyPipelines(device, pipelines);
    vk::DestroyDevice(device);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
