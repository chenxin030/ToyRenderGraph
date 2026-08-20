#pragma once

#include "rg/resource.h"

namespace rg {

// 由编译自动推导的资源状态转换，最终由后端插入为屏障。
struct Transition {
    ResourceHandle handle;
    ResourceState oldState = ResourceState::Undefined;  // 转换前状态；Undefined = 丢弃内容
    ResourceState newState = ResourceState::ShaderRead;  // 转换后状态

    // 别名交接：本资源的首次写入发生在一个已被其他资源使用过的内存槽上。
    // 必须等前一资源对该内存的最后一次访问结束，否则会读写同一块显存。
    bool aliasHandoff = false;
    ResourceState aliasSrcState = ResourceState::Undefined;  // 前一资源最后的访问状态
};

}  // namespace rg
