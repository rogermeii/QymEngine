# Metal 后端设计

## 概述

为 QymEngine 添加原生 Metal 渲染后端，使 macOS 不再依赖 MoltenVK 翻译层。
遵循现有 VkDispatch 模式，实现全部 91 个 Vulkan API 函数指针的 Metal 翻译。
功能与 OpenGL 后端完整对等：场景渲染、ImGui、后处理、IBL。

## 方案选择

- **着色器路径**: 方案 B — 离线编译 MSL 变体（Slang → SLANG_METAL → MSL 文本）
- **文件结构**: 单文件方案 — `VkMetal.mm` + `VkMetalHandles.h`，与现有后端风格一致
- **功能范围**: 完整对等 — 一步到位实现全部功能

## 文件结构

```
engine/renderer/metal/
├── VkMetalHandles.h    — MTL_ 前缀句柄结构体
└── VkMetal.mm          — 全部 Vulkan API 的 Metal 实现 + vkLoadMetalDispatch()
```

## 句柄映射

| Vulkan 句柄 | Metal 结构体 | 核心 Metal 对象 |
|---|---|---|
| VkInstance | MTL_Instance | — |
| VkPhysicalDevice | MTL_PhysicalDevice | — |
| VkDevice | MTL_Device | `id<MTLDevice>` |
| VkQueue | MTL_Queue | `id<MTLCommandQueue>` |
| VkSurfaceKHR | MTL_Surface | `CAMetalLayer*` |
| VkSwapchainKHR | MTL_Swapchain | `CAMetalLayer*` + `id<CAMetalDrawable>` |
| VkBuffer | MTL_Buffer | `id<MTLBuffer>` |
| VkDeviceMemory | MTL_Memory | `id<MTLBuffer>` (共享内存模型) |
| VkImage | MTL_Image | `id<MTLTexture>` |
| VkImageView | MTL_ImageView | `id<MTLTexture>` (texture view) |
| VkSampler | MTL_Sampler | `id<MTLSamplerState>` |
| VkShaderModule | MTL_ShaderModule | MSL 源码字符串 |
| VkPipeline | MTL_Pipeline | `id<MTLRenderPipelineState>` + `id<MTLDepthStencilState>` |
| VkPipelineLayout | MTL_PipelineLayout | set layout 引用 + push constant 范围 |
| VkRenderPass | MTL_RenderPass | attachment 描述 |
| VkFramebuffer | MTL_Framebuffer | attachment texture 引用 |
| VkCommandPool | MTL_CommandPool | — |
| VkCommandBuffer | MTL_CommandBuffer | `id<MTLCommandBuffer>` + `id<MTLRenderCommandEncoder>` |
| VkDescriptorSetLayout | MTL_DescriptorSetLayout | binding 布局描述 |
| VkDescriptorPool | MTL_DescriptorPool | — |
| VkDescriptorSet | MTL_DescriptorSet | buffer/texture 绑定数组 |
| VkFence | MTL_Fence | `dispatch_semaphore_t` |
| VkSemaphore | MTL_Semaphore | — (Metal 自动排序) |

## 着色器路径

```
Slang (.slang)
  → ShaderCompiler (SLANG_METAL)
  → MSL 文本
  → shaderbundle "default_msl" 变体
  → 运行时: MTLDevice.newLibrary(source:) → MTLFunction
  → MTLRenderPipelineState
```

- 变体名: `"default_msl"`
- ImGui 着色器: 检测 SPIR-V 魔数，替换为内置 MSL（同 OpenGL 做法）
- 反射数据: 复用 SPIR-V 变体的反射 JSON

## 关键实现细节

### 内存模型

Metal 的 `MTLBuffer` 创建时即分配内存，不需要独立 alloc/bind 流程。
`MTL_Memory` 作为薄封装，`vkBindBufferMemory` 为空操作。

### 渲染流程

```
vkBeginCommandBuffer  → 创建 MTLCommandBuffer
vkCmdBeginRenderPass  → 构建 MTLRenderPassDescriptor，创建 MTLRenderCommandEncoder
vkCmdBindPipeline     → setRenderPipelineState + setDepthStencilState
vkCmdBindVertexBuffers→ setVertexBuffer
vkCmdBindDescriptorSets → setVertexBuffer/setFragmentBuffer/setFragmentTexture
vkCmdPushConstants    → setVertexBytes / setFragmentBytes
vkCmdDraw/DrawIndexed → drawPrimitives / drawIndexedPrimitives
vkCmdEndRenderPass    → endEncoding
vkEndCommandBuffer    → 记录完成
vkQueueSubmit         → commit
vkQueuePresentKHR     → presentDrawable
```

### Swapchain

使用 SDL2 Metal 支持: `SDL_Metal_GetLayer()` 获取 `CAMetalLayer*`。
每帧通过 `nextDrawable` 获取可绘制纹理。

### 坐标系

Metal NDC 深度范围 `[0,1]`，Y 轴向上，与 Vulkan 约定一致（`GLM_FORCE_DEPTH_ZERO_TO_ONE`）。
不需要坐标翻转。

### 格式转换

建立 `VkFormat → MTLPixelFormat` 映射表，覆盖引擎使用的所有格式：
- `VK_FORMAT_B8G8R8A8_SRGB` → `MTLPixelFormatBGRA8Unorm_sRGB`
- `VK_FORMAT_R8G8B8A8_UNORM` → `MTLPixelFormatRGBA8Unorm`
- `VK_FORMAT_D32_SFLOAT` → `MTLPixelFormatDepth32Float`
- 等

## 构建集成

### CMake 改动

- `engine/CMakeLists.txt`: macOS 上编译 `metal/VkMetal.mm`，链接 `-framework Metal -framework QuartzCore`
- macOS 上排除 OpenGL 后端的 GLAD 依赖（可选，两个后端可共存）

### VkDispatch 改动

- `VkDispatch.h`: 添加 `vkIsMetalBackend()` 查询
- `VkDispatch.cpp`: `backendType == 5` → `vkLoadMetalDispatch()`

### 窗口/入口改动

- `Window.h`: `RenderBackend` 枚举加 `Metal`
- `Window.cpp`: Metal 后端使用 `SDL_WINDOW_METAL` 标志创建窗口
- `editor/main.cpp`: 加 `--metal` 命令行参数
- `AssetManager.cpp`: 变体选择加 `vkIsMetalBackend() ? "default_msl" : ...`

### 着色器编译器改动

- `tools/shader_compiler/main.cpp`:
  - 加 `g_emitMsl` 标志
  - 加 `SLANG_METAL` 编译分支
  - 输出 `"default_msl"` 变体
  - 复用 SPIR-V 反射数据

## 实现顺序

1. 构建基础设施（CMake、VkDispatch 集成、Window、枚举）
2. VkMetalHandles.h（全部句柄结构体）
3. VkMetal.mm 核心：instance/device/queue/surface/swapchain
4. VkMetal.mm 资源：buffer/memory/image/imageview/sampler
5. VkMetal.mm 管线：shader module/pipeline layout/pipeline/render pass/framebuffer
6. VkMetal.mm 命令：command pool/buffer/录制/提交
7. VkMetal.mm 描述符：descriptor set layout/pool/set/update
8. VkMetal.mm 同步：fence/semaphore
9. ImGui MSL 着色器替换
10. 着色器编译器 MSL 变体支持
11. 集成测试：编辑器和独立运行时
