#pragma once

#include "rg/barrier.h"
#include "rg/render_graph.h"

#include <algorithm>
#include <cstdio>
#include <queue>
#include <vector>

namespace rg {

// 编译后的一个 Pass，按拓扑顺序排列。
struct CompiledPass {
    uint32_t passIndex = 0;
    std::vector<Transition> before;  // 执行本 Pass 前插入的屏障
    std::vector<Transition> after;   // 执行本 Pass 后插入的屏障（如呈现交换链）
};

// 一个内存槽：生命周期互不重叠的一组瞬时资源，共享一块物理内存。
struct MemorySlot {
    std::vector<ResourceHandle> resources;  // 按首次使用排序
};

struct CompiledGraph {
    std::vector<CompiledPass> passes;   // 拓扑顺序
    std::vector<MemorySlot> slots;      // 显存复用计划（仅瞬时资源）
    std::vector<uint32_t> stripped;     // 被死枝剔除的 Pass（声明顺序）
};

// 编译一帧：死枝剔除 -> 拓扑排序 -> 屏障推导 -> 显存复用。
// 纯逻辑，不需要图形 API。
inline CompiledGraph Compile(const std::vector<Pass>& passes,
                             const std::vector<Resource>& resources,
                             const std::vector<ResourceHandle>& outputResources)// 这一帧结束时，哪些资源必须有效，也是死枝剔除的起点
{  
    const uint32_t passCount = static_cast<uint32_t>(passes.size());
    const uint32_t resCount = static_cast<uint32_t>(resources.size());

    // 资源生命周期
    std::vector<Lifetime> lifetimes(resCount);
    for (uint32_t p = 0; p < passCount; ++p) {
        for (const auto& resourceBinding : passes[p].bindings) {
            if (!resourceBinding.handle.valid() || resourceBinding.handle.index >= resCount) continue;
            auto& lf = lifetimes[resourceBinding.handle.index];
            lf.firstUse = std::min(lf.firstUse, p);
            lf.lastUse = std::max(lf.lastUse, p);
        }
    }

    // 死枝剔除：从最后一个pass往前倒找出不需要的pass
    std::vector<bool> needed(passCount, false);
    std::vector<bool> consumed(resCount, false);  // 哪些资源被使用
    for (const auto& resource : outputResources) {
        if (resource.valid() && resource.index < resCount) consumed[resource.index] = true;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t p = 0; p < passCount; ++p) {
            if (needed[p]) continue;
            for (const auto& b : passes[p].bindings) {
                if (b.write && consumed[b.handle.index]) {
                    needed[p] = true;
                    changed = true;
                    for (const auto& b : passes[p].bindings) {
                        if (!b.write) consumed[b.handle.index] = true;
                    }
                    break;
                }
            }
        }
    }
    for (uint32_t p = 0; p < passCount; ++p) {
        if (!needed[p]) {
            std::printf("[Compile] dead-strip: pass '%s'\n", passes[p].name.c_str());
        }
    }

    // 拓扑排序（Kahn 算法）
    // 边规则：写资源 X 的 Pass -> 读 X 的 Pass；后写的覆盖先写的。
    std::vector<uint32_t> indegree(passCount, 0);           // 每个 Pass 的入度
    std::vector<std::vector<uint32_t>> deps(passCount);     // deps[p] = p 依赖的前驱 Pass 列表
    std::vector<uint32_t> lastWriter(resCount, UINT32_MAX); // 每个资源"最近一次写它的 Pass"
    for (uint32_t p = 0; p < passCount; ++p) {
        if (!needed[p]) continue;
        for (const auto& b : passes[p].bindings) {
            const uint32_t r = b.handle.index;
            if (r >= resCount) continue;
            if (lastWriter[r] != UINT32_MAX && lastWriter[r] != p) {
                const uint32_t w = lastWriter[r];
                if (std::find(deps[p].begin(), deps[p].end(), w) == deps[p].end()) {
                    deps[p].push_back(w);
                    ++indegree[p];
                }
            }
            if (b.write) lastWriter[r] = p;
        }
    }
    std::queue<uint32_t> ready;
    for (uint32_t p = 0; p < passCount; ++p) {
        if (needed[p] && indegree[p] == 0) ready.push(p);
    }
    std::vector<uint32_t> order;
    order.reserve(passCount);
    while (!ready.empty()) {
        const uint32_t p = ready.front();
        ready.pop();
        order.push_back(p);
        for (uint32_t q = 0; q < passCount; ++q) {
            if (!needed[q]) continue;
            if (std::find(deps[q].begin(), deps[q].end(), p) != deps[q].end()) {
                if (--indegree[q] == 0) ready.push(q);
            }
        }
    }
    if (order.size() != static_cast<size_t>(std::count(needed.begin(), needed.end(), true))) {
        std::printf("[Compile] WARNING: cycle detected, fallback to declaration order\n");
        order.clear();
        for (uint32_t p = 0; p < passCount; ++p) {
            if (needed[p]) order.push_back(p);
        }
    }

    // 显存复用：早出现的资源先分 slot
    std::vector<int32_t> slotOf(resCount, -1);   // 资源被分到几号 slot，-1 = 还没分/不参与
    std::vector<MemorySlot> slots;               // 每个 slot 记录里面放了哪些资源
    std::vector<uint32_t> transients;            // 参与复用的候选资源集合
    for (uint32_t r = 0; r < resCount; ++r) {
        if (resources[r].transient && lifetimes[r].firstUse != UINT32_MAX) {// 内部创建且被用过
            transients.push_back(r);
        }
    }
    // 按 firstUse 升序排序（相同时按 index）
    std::sort(transients.begin(), transients.end(), [&](uint32_t a, uint32_t b) {
        return lifetimes[a].firstUse != lifetimes[b].firstUse ? lifetimes[a].firstUse < lifetimes[b].firstUse : a < b;
    });
    for (const uint32_t r : transients) {
        int32_t slot = -1;
        // 遍历已存在的 slot，找出 r 和 slot 里资源的生命周期不重叠的 slot 把 r 放进去
        for (size_t s = 0; s < slots.size(); ++s) {
            bool free = true;
            for (const auto& other : slots[s].resources) {
                if (lifetimes[r].overlaps(lifetimes[other.index])) {
                    free = false;
                    break;
                }
            }
            if (free) {
                slot = static_cast<int32_t>(s);
                break;
            }
        }
        // 找不到就开新 slot
        if (slot < 0) {
            slots.push_back(MemorySlot{});
            slot = static_cast<int32_t>(slots.size() - 1);
        }
        // 登记
        slots[static_cast<size_t>(slot)].resources.push_back(ResourceHandle{r});
        slotOf[r] = slot;
    }

    // 屏障推导
    std::vector<ResourceState> currentState(resCount, ResourceState::Undefined);
    std::vector<uint32_t> slotOwner(slots.size(), UINT32_MAX);        // 当前占用该槽的资源
    std::vector<ResourceState> slotLastState(slots.size(), ResourceState::Undefined);

    std::vector<CompiledPass> compiled;
    compiled.reserve(order.size());
    for (const uint32_t p : order) {
        CompiledPass cp;
        cp.passIndex = p;
        for (const auto& b : passes[p].bindings) {
            const uint32_t r = b.handle.index;
            if (!b.handle.valid() || r >= resCount) continue;
            const ResourceState oldState = currentState[r];
            ResourceState newState = b.state;
            if (b.write) {
                // 写入状态由资源格式决定：深度格式 -> 深度附件，其余 -> 颜色附件
                newState = (resources[r].desc.format == Format::D32_SFLOAT)
                               ? ResourceState::DepthWrite
                               : ResourceState::ColorWrite;
            }

            const bool firstUse = (oldState == ResourceState::Undefined);
            const int32_t slot = slotOf[r];

            if (firstUse && resources[r].transient && !b.write) {
                std::printf("[Compile] WARNING: resource '%s' is read before any write\n",
                            resources[r].name.c_str());
            }
            if (firstUse) {
                // 首次使用：内容未定义，无需等待任何先前的访问（src=TOP_OF_PIPE），
                // 但仍需要一次"丢弃内容 + 布局转换"屏障，把图像切到目标 layout。
                // 若内存槽曾被其他资源占用，还要等待前一资源的最后一次访问（别名交接）。
                Transition t{b.handle, ResourceState::Undefined, newState, false,
                             ResourceState::Undefined};
                if (resources[r].transient && slot >= 0 &&
                    slotOwner[static_cast<size_t>(slot)] != UINT32_MAX) {
                    t.aliasHandoff = true;
                    t.aliasSrcState = slotLastState[static_cast<size_t>(slot)];
                }
                cp.before.push_back(t);
            } else if (oldState != newState) {
                // 常规状态转换：内容需要保留，做一次有意义的 layout/access 转换。
                Transition t{b.handle, oldState, newState, false, ResourceState::Undefined};
                cp.before.push_back(t);
            }

            if (firstUse && slot >= 0) {
                slotOwner[static_cast<size_t>(slot)] = r;
            }
            if (slot >= 0) {
                slotLastState[static_cast<size_t>(slot)] = newState;
            }
            currentState[r] = newState;
        }
        compiled.push_back(std::move(cp));
    }

    // 转换：外部资源在最后一次写入后切到 Present
    if (!compiled.empty()) {
        for (uint32_t r = 0; r < resCount; ++r) {
            const ResourceState s = currentState[r];
            if (!resources[r].transient && (s == ResourceState::ColorWrite || s == ResourceState::DepthWrite)) {
                Transition t{ResourceHandle{r}, s, ResourceState::Present, false, ResourceState::Undefined};
                compiled.back().after.push_back(t);
            }
        }
    }

    CompiledGraph result;
    result.passes = std::move(compiled);
    result.slots = std::move(slots);
    for (uint32_t p = 0; p < passCount; ++p) {
        if (!needed[p]) result.stripped.push_back(p);
    }
    return result;
}

}  // namespace rg
