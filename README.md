# ToyRenderGraph

简易的 RenderGraph：用声明式 Pass 依赖图驱动一条 4 个 Pass 渲染管线，自动完成**排序、同步屏障和显存复用**。

```
Shadow ──写──▶ ShadowMap ──读──▶ Scene ──写──▶ HDRColor ──┬──读──▶ Present ──▶ Swapchain
                                    └──写──▶ SceneDepth     └──读──▶ Blur ──写──▶ BlurBuffer ──┘
```

## 文件

```
src/rg/      图核心：纯逻辑，零 Vulkan 类型（header-only）
  render_graph.h  声明式 API：AddPass<Data>(setup, execute)
  compile.h       死枝剔除 + 拓扑排序 + 屏障推导 + 显存复用（贪心区间调度）
src/vk/      Vulkan 后端
  vk_backend      把编译计划翻译为 vkCmdPipelineBarrier2 与 dynamic rendering
  vk_resources    TransientPool：每个内存槽一块显存，槽内资源按生命周期共享
  vk_pipeline     4 条图形管线与描述符集合
src/demo/    4 个 Pass 与程序化场景（地面 + 3 个彩色立方体 + 平行光阴影）
```

## 六大能力

1. **声明式 API**：`AddPass<ShadowData>("Shadow", setup, execute)`，setup 声明意图、execute 在编译后拿到物理资源——"延迟执行"直接用代码讲出来。
2. **拓扑排序**：Kahn 算法自动推导 Pass 执行顺序。
3. **自动屏障**：逐资源状态追踪（layout/access），自动插入 `vkCmdPipelineBarrier2`。
4. **瞬时资源与生命周期**：资源存活区间 = 首次写入到末次读取。
5. **死枝剔除**：不贡献最终输出的 Pass 自动移除。
6. **显存复用**：生命周期不重叠的瞬时资源共享同一块显存。本 demo 中 `ShadowMap` 与 `BlurBuffer` 共享 16MB；启动日志会打印每个槽的别名计划。

## 构建

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
.\build\Debug\ToyRenderGraph.exe
```
