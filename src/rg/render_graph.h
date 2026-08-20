#pragma once

#include "rg/resource.h"

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace rg {

class RenderGraph;

// Setup 阶段的操作面：上层只能通过它声明资源与依赖，
// 不能直接触碰物理资源——隔开"声明期"与"执行期"的边界。
class PassBuilder {
public:
    // 在图内创建瞬时资源，返回句柄
    ResourceHandle CreateTexture(const TextureDesc& desc, const std::string& name);
    // 声明本 Pass 采样读取一个资源
    void Read(ResourceHandle handle);
    // 声明本 Pass 写入一个资源（颜色/深度附件）
    void Write(ResourceHandle handle);

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& graph, uint32_t passIndex);

    RenderGraph& m_graph;
    uint32_t m_passIndex;
};

// 图中的一个 Pass：名称、资源绑定、具体要做什么。
struct Pass {
    std::string name;
    std::vector<ResourceBinding> bindings;

    // 类型擦除：图核心不依赖 Vulkan 类型，执行时由后端把具体上下文指针传入。
    std::function<void(void*)> execute;
};

// PassData 类型推导：让 AddPass 不必显式写出模板参数。
// 从 setup 回调签名 void(PassBuilder&, T&) 中取出 T。
template <typename F>
struct SetupTraits;
template <typename T>
struct SetupTraits<std::function<void(PassBuilder&, T&)>> {
    using type = T;
};
template <typename T>
struct SetupTraits<void (*)(PassBuilder&, T&)> {
    using type = T;
};
template <typename R, typename C, typename T>
struct SetupTraits<R (C::*)(PassBuilder&, T&) const> {
    using type = T;
};
template <typename R, typename C, typename T>
struct SetupTraits<R (C::*)(PassBuilder&, T&)> {
    using type = T;
};
template <typename F>
struct SetupTraits : SetupTraits<decltype(&F::operator())> {};

// 从 execute 回调签名 void(const T&, ExecCtx&) 中取出 T 与 ExecCtx。
template <typename F>
struct ExecuteTraits;
template <typename T, typename ExecCtx>
struct ExecuteTraits<std::function<void(const T&, ExecCtx&)>> {
    using TData = T;
    using TCtx = ExecCtx;
};
template <typename T, typename ExecCtx>
struct ExecuteTraits<void (*)(const T&, ExecCtx&)> {
    using TData = T;
    using TCtx = ExecCtx;
};
template <typename R, typename C, typename T, typename ExecCtx>
struct ExecuteTraits<R (C::*)(const T&, ExecCtx&) const> {
    using TData = T;
    using TCtx = ExecCtx;
};
template <typename R, typename C, typename T, typename ExecCtx>
struct ExecuteTraits<R (C::*)(const T&, ExecCtx&)> {
    using TData = T;
    using TCtx = ExecCtx;
};
template <typename F>
struct ExecuteTraits : ExecuteTraits<decltype(&F::operator())> {};

// RenderGraph：一帧的完整声明。
// 使用流程固定为三阶段：AddPass（Setup）→ Compile -> Execute。
class RenderGraph {
public:
    RenderGraph() = default;

    // 导入外部资源（如交换链图像）：图只追踪它的使用，不管理生命周期。
    ResourceHandle Import(const TextureDesc& desc, const std::string& name);

    // 创建瞬时资源
    ResourceHandle CreateTexture(const TextureDesc& desc, const std::string& name);

    // setup：立即执行，通过 PassBuilder 声明资源依赖，填充 data；
    // execute：编译完成后执行，拿到的是被解析为物理资源的 data。
    template <typename Setup, typename Execute>
    void AddPass(const std::string& name, Setup setup, Execute execute) {
        using T = typename SetupTraits<Setup>::type;
        using ExecCtx = typename ExecuteTraits<Execute>::TCtx;
        static_assert(std::is_same_v<T, typename ExecuteTraits<Execute>::TData>,
                      "setup 与 execute 的 PassData 类型必须一致。");

        // 先占位：PassBuilder 在 setup 阶段写入 m_passes[index]；
        // 必须保证该位置已存在（否则越界访问）。
        m_passes.push_back(Pass{name, {}, {}});
        auto data = std::make_shared<T>();
        {
            PassBuilder builder(*this, static_cast<uint32_t>(m_passes.size()) - 1);
            setup(builder, *data);
        }
        m_passes.back().execute = [data, execute](void* rawCtx) {
            execute(*data, *static_cast<ExecCtx*>(rawCtx));
        };
    }

    const std::vector<Pass>& passes() const { return m_passes; }
    const std::vector<Resource>& resources() const { return m_resources; }

private:
    friend class PassBuilder;

    std::vector<Pass> m_passes;
    std::vector<Resource> m_resources;
};

inline PassBuilder::PassBuilder(RenderGraph& graph, uint32_t passIndex)
    : m_graph(graph), m_passIndex(passIndex) {}

inline ResourceHandle PassBuilder::CreateTexture(const TextureDesc& desc, const std::string& name) {
    return m_graph.CreateTexture(desc, name);
}

inline void PassBuilder::Read(ResourceHandle handle) {
    m_graph.m_passes[m_passIndex].bindings.push_back(
        {handle, false, ResourceState::ShaderRead});
}

inline void PassBuilder::Write(ResourceHandle handle) {
    // 具体写入状态（颜色/深度）由资源的格式决定，见 Compile。
    m_graph.m_passes[m_passIndex].bindings.push_back(
        {handle, true, ResourceState::Undefined});
}

inline ResourceHandle RenderGraph::Import(const TextureDesc& desc, const std::string& name) {
    m_resources.push_back({name, desc, false});
    return ResourceHandle{static_cast<uint32_t>(m_resources.size() - 1)};
}

inline ResourceHandle RenderGraph::CreateTexture(const TextureDesc& desc, const std::string& name) {
    m_resources.push_back({name, desc, true});
    return ResourceHandle{static_cast<uint32_t>(m_resources.size() - 1)};
}

}  // namespace rg
