#pragma once

#include <cstdint>
#include <string>

namespace rg {

// 抽象资源格式：图核心不关心具体图形 API 的格式枚举，
// 由后端（Vulkan）负责映射。
enum class Format {
    R8G8B8A8_UNORM,
    R16G16B16A16_SFLOAT,
    D32_SFLOAT,
};

// 抽象资源状态：表示"这个资源此刻处于什么使用方式"。
// 后端把它翻译成 Vulkan 的 image layout / pipeline stage / access mask。
enum class ResourceState {
    Undefined,    // 内容未知，可以被丢弃
    ColorWrite,   // 作为颜色附件写入
    DepthWrite,   // 作为深度附件写入
    ShaderRead,   // 在 fragment shader 中采样读取
    Present,      // 交给交换链呈现
};

// 纹理描述：一张纹理的宽、高与格式。图核心只用它声明资源，
// 由后端（Vulkan）映射为实际的图像对象。
struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::R8G8B8A8_UNORM;
};

struct Resource {
    std::string name;
    TextureDesc desc;
    // transient = 图内创建、生命周期由图管理；false = 外部导入（如交换链图像）
    bool transient = true;
};

// 图内资源的轻量句柄：本质是 resources 数组的下标。
// 上层用它声明依赖，编译后由后端解析为物理资源。
struct ResourceHandle {
    uint32_t index = UINT32_MAX;

    bool valid() const { return index != UINT32_MAX; }
};


// 一个 Pass 对某个资源的一次使用：读还是写，以及期望的状态。
struct ResourceBinding {
    ResourceHandle handle;
    bool write = false;
    ResourceState state = ResourceState::ShaderRead;
};

// 资源生命周期：从首次使用到末次使用的 Pass 区间（含两端）。
// 区间互不重叠的资源才能共享同一块物理显存。
struct Lifetime {
    uint32_t firstUse = UINT32_MAX;
    uint32_t lastUse = 0;

    bool overlaps(const Lifetime& other) const {
        return firstUse <= other.lastUse && other.firstUse <= lastUse;
    }
};

}  // namespace rg
