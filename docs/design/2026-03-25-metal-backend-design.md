# Metal 后端设计与实现

## 概述

为 QymEngine 添加原生 Metal 渲染后端，支持 macOS 和 iOS 平台。
遵循现有 VkDispatch 模式，实现全部 91 个 Vulkan API 函数指针的 Metal 翻译。
功能与 OpenGL/Vulkan 后端完整对等：场景渲染、ImGui、后处理、IBL、Grid。

## 最终方案

- **着色器路径**: Slang 离线编译 MSL 变体（`SLANG_METAL` 目标），无运行时依赖
- **文件结构**: 单文件方案 — `VkMetal.mm` + `VkMetalHandles.h`
- **功能范围**: 完整对等 — macOS 和 iOS 真机均可运行编辑器
- **平台支持**: macOS (Apple Silicon) + iOS 15.0+ 真机

## 文件结构

```
engine/renderer/metal/
├── VkMetalHandles.h    — MTL_ 前缀句柄结构体（22 个）
└── VkMetal.mm          — 全部 Vulkan API 的 Metal 实现 + vkLoadMetalDispatch()
                          + parseMSLMetadata() MSL 文本解析
                          + 内置 ImGui MSL 着色器

ios-cmake/
├── CMakeLists.txt      — iOS Xcode 项目生成（SDL2 + SPIRV-Cross FetchContent）
├── main.mm             — iOS 入口（SDL_main, Metal 后端）
├── Info.plist.in       — iOS app 属性（横屏、全屏）
└── LaunchScreen.storyboard — 启动画面
```

## 句柄映射

| Vulkan 句柄 | Metal 结构体 | 核心 Metal 对象 |
|---|---|---|
| VkInstance | MTL_Instance | — |
| VkPhysicalDevice | MTL_PhysicalDevice | `id<MTLDevice>` (via MTLCreateSystemDefaultDevice) |
| VkDevice | MTL_Device | `id<MTLDevice>` + `id<MTLCommandQueue>` |
| VkQueue | MTL_Queue | device 引用 |
| VkSurfaceKHR | MTL_Surface | `SDL_Window*` + `CAMetalLayer*` |
| VkSwapchainKHR | MTL_Swapchain | `CAMetalLayer*` + `id<CAMetalDrawable>` |
| VkBuffer | MTL_Buffer | `id<MTLBuffer>` (MTLResourceStorageModeShared) |
| VkDeviceMemory | MTL_Memory | 薄封装（boundBuffer/boundImage 关联） |
| VkImage | MTL_Image | `id<MTLTexture>` |
| VkImageView | MTL_ImageView | `id<MTLTexture>` (texture view) |
| VkSampler | MTL_Sampler | `id<MTLSamplerState>` |
| VkShaderModule | MTL_ShaderModule | MSL 源码 + parseMSLMetadata 解析的绑定信息 |
| VkPipeline | MTL_Pipeline | `id<MTLRenderPipelineState>` + `id<MTLDepthStencilState>` + 光栅化状态 + VS/FS 绑定映射 |
| VkPipelineLayout | MTL_PipelineLayout | set layout 引用 + push constant 范围 |
| VkRenderPass | MTL_RenderPass | attachment 描述 vector |
| VkFramebuffer | MTL_Framebuffer | attachment VkImageView 引用 |
| VkCommandPool | MTL_CommandPool | device 引用 |
| VkCommandBuffer | MTL_CommandBuffer | `id<MTLCommandBuffer>` + `id<MTLRenderCommandEncoder>` + 绑定状态 |
| VkDescriptorSetLayout | MTL_DescriptorSetLayout | binding 描述 vector |
| VkDescriptorPool | MTL_DescriptorPool | 薄封装 |
| VkDescriptorSet | MTL_DescriptorSet | buffer/texture/sampler 绑定数组 |
| VkFence | MTL_Fence | `dispatch_semaphore_t` |
| VkSemaphore | MTL_Semaphore | 空操作（Metal 自动排序） |

## 着色器路径

```
Slang (.slang) → ShaderCompiler (SLANG_METAL + __METAL__ 宏)
    → MSL 文本 → shaderbundle "default_msl" 变体
    → 运行时: parseMSLMetadata() 解析 entry point 和绑定信息
    → MTLDevice.newLibrary(source:) → MTLFunction
    → MTLRenderPipelineState
```

### MSL 文本解析 (parseMSLMetadata)

从 Slang 生成的 MSL 文本中用正则表达式解析：
- **Entry point 名**: `[[vertex]]`/`[[fragment]]` 后的函数名（如 `vertexMain`、`fragmentMain`）
- **Buffer index**: `[[buffer(N)]]` 注解 → UBO 和 push constant 的 Metal buffer index
- **Texture/Sampler index**: `[[texture(N)]]`/`[[sampler(N)]]` 注解
- **顶点缓冲 index**: max(所有 buffer index) + 1

### Slang MSL 的 buffer index 分配规则

Slang 将所有 descriptor set 的 binding 扁平化分配 Metal index：
- 所有 UBO 按 (set, binding) 排序后依次分配 buffer(0), buffer(1), ...
- Push constants 分配下一个 buffer index
- Texture/sampler 同理按 (set, binding) 排序分配 texture(0), texture(1), ...
- 顶点缓冲不在 MSL 签名中（使用 `[[stage_in]]`），通过 vertex descriptor 绑定到 max+1 index

### ImGui 着色器

ImGui 的 `imgui_impl_vulkan` 传入 SPIR-V，Metal 后端检测 SPIR-V 魔数后替换为内置 MSL：
- 顶点着色器: `imgui_vertex` — projection matrix 变换 + Y 轴翻转
- 片段着色器: `imgui_fragment` — 纹理采样 × 顶点颜色
- 固定绑定: buffer(0)=顶点缓冲, buffer(1)=uniforms, texture(0)/sampler(0)=字体图集

## 关键实现细节

### 内存模型

Metal 的 `MTLBuffer` 使用 `MTLResourceStorageModeShared`，创建时即分配 CPU/GPU 共享内存。
- `vkAllocateMemory`: 创建 `MTL_Memory` 薄封装
- `vkBindBufferMemory`: 记录 buffer ↔ memory 关联（`MTL_Memory.boundBuffer`）
- `vkMapMemory`: 通过 memory 找到关联 buffer，返回 `[buffer contents]` + offset
- `vkBindImageMemory`: 空操作（texture 创建时已分配内存）

### 渲染流程

```
vkBeginCommandBuffer  → [commandQueue commandBuffer]
vkCmdBeginRenderPass  → 构建 MTLRenderPassDescriptor + [commandBuffer renderCommandEncoderWithDescriptor:]
                        （swapchain image 用当前 drawable.texture）
vkCmdBindPipeline     → setRenderPipelineState + setDepthStencilState + setCullMode + setFrontFacingWinding
vkCmdBindVertexBuffers→ [encoder setVertexBuffer:offset:atIndex:vertexBufferIndex]
vkCmdBindDescriptorSets → 遍历绑定：setVertexBuffer/setFragmentBuffer (UBO)
                          + setFragmentTexture + setFragmentSamplerState (texture)
vkCmdPushConstants    → [encoder setVertexBytes/setFragmentBytes:atIndex:pushConstIndex]
vkCmdDraw/DrawIndexed → drawPrimitives / drawIndexedPrimitives
vkCmdEndRenderPass    → [encoder endEncoding]
vkCmdCopyBuffer/Image → 创建 MTLBlitCommandEncoder（如有活跃 render encoder 先 endEncoding）
vkQueueSubmit         → [commandBuffer presentDrawable:] (仅帧提交) + [commandBuffer commit]
```

### Swapchain

- SDL2 Metal 支持: `SDL_Metal_CreateView()` + `SDL_Metal_GetLayer()` 获取 `CAMetalLayer*`
- 每帧 `[layer nextDrawable]` 获取 `id<CAMetalDrawable>`
- `maximumDrawableCount = 3` (triple buffering)
- macOS: `displaySyncEnabled = YES` (VSync)
- iOS: 默认 VSync

### 坐标系与 Y 轴翻转

Vulkan NDC Y 向下，Metal NDC Y 向上。采用**负高度 viewport** 方案统一处理：
```objc
vp.originY = y + h;  // 从底边开始
vp.height = -h;      // 负高度 = Y 向上
```
这样所有着色器（包括 ImGui）不需要在代码中翻转 Y。Metal 2.0+ (macOS 10.15+, iOS 13+) 支持负高度 viewport。

### 同步

- `MTL_Fence`: 使用 `dispatch_semaphore_t`
  - `vkWaitForFences`: `dispatch_semaphore_wait`
  - fence 在 command buffer completion handler 中 signal
- `MTL_Semaphore`: 空操作（Metal command buffer 提交自动排序）
- `s_lastCommittedBuffer`: 全局变量跟踪最后提交的帧 command buffer
  - 仅帧提交（有 wait semaphore）才更新，single-time command 不影响
  - `acquireNextImage` 中 `[s_lastCommittedBuffer waitUntilCompleted]` 确保 drawable 回收

## 构建集成

### macOS (engine/CMakeLists.txt)

```cmake
if(APPLE)
    file(GLOB METAL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/renderer/metal/*.mm")
    set_source_files_properties(${METAL_SOURCES} PROPERTIES COMPILE_FLAGS "-fobjc-arc -xobjective-c++")
    target_link_libraries(QymEngineLib PUBLIC "-framework Metal" "-framework QuartzCore")
endif()
```

### iOS (ios-cmake/CMakeLists.txt)

- CMake `-G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64`
- SDL2 via FetchContent (静态库)
- 资源文件打包到 `MACOSX_BUNDLE` 的 `Resources/assets/`
- 签名: Team `D3S7B866Z2`, Automatic signing, iOS 15.0+

### VkDispatch 集成

- `RenderBackend::Metal` 枚举值, `backendType = 5`
- `vkIsMetalBackend()` 查询函数
- `vkLoadMetalDispatch()` 注册全部函数指针
- iOS 上排除 OpenGL 后端（`#if !TARGET_OS_IOS`）

### 着色器编译器

- `g_emitMsl` 标志 + `SLANG_METAL` 目标
- 编译时传入 `__METAL__` 预处理宏
- 输出 `"default_msl"` 变体，复用 SPIR-V 反射数据
- `SLANG_SDK_PATH` CMake 变量支持独立 Slang SDK

## 遇到的问题与解决方案

### 1. macOS 构建适配

**问题**: 项目原本只支持 Windows，macOS 上无法编译。
**解决**:
- `CMakeLists.txt` 加 C 语言支持（SDL2 需要）
- 排除 D3D11/D3D12 后端（`list(FILTER ... EXCLUDE ...)`）
- `VK_USE_PLATFORM_MACOS_MVK` 替代 `VK_USE_PLATFORM_WIN32_KHR`
- GCC/Clang 用 `-include` 替代 MSVC 的 `/FI`
- 字体路径适配（`/System/Library/Fonts/Hiragino Sans GB.ttc`）
- RenderDoc 代码用 `#ifdef _WIN32` 条件编译

### 2. MoltenVK 环境配置

**问题**: macOS Vulkan 需要 MoltenVK 但 SDL2 找不到 Vulkan 库。
**解决**: 需要设置环境变量：
```bash
DYLD_LIBRARY_PATH=/opt/homebrew/lib
VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
SDL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.dylib
```

### 3. Vulkan Instance 创建失败

**问题**: macOS MoltenVK 需要 portability enumeration 扩展。
**解决**: `VulkanContext::createInstance()` 中加 `VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME` + `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`。

### 4. ShaderBundle 路径问题

**问题**: iOS 上 `SDL_RWFromFile` 的路径去除前缀逻辑破坏了绝对路径。
**解决**: 路径清理仅在 `#ifdef __ANDROID__` 下执行。

### 5. ImGui 顶点格式不匹配

**问题**: ImGui 内置 MSL 着色器声明 `uchar4 color` 但 pipeline 使用 `UChar4Normalized`。
**解决**: MSL 中改为 `float4 color`（`UChar4Normalized` 自动转为 float）。

### 6. UI 上下颠倒

**问题**: Vulkan NDC Y 向下，Metal Y 向上，画面颠倒。
**经历**: 先尝试 SPIRV-Cross `flip_vert_y` + ImGui 手动翻转 Y，效果不理想。
**最终方案**: 负高度 viewport（`height = -h, originY = y + h`），统一处理所有着色器。

### 7. SPIRV-Cross 在 iOS 上崩溃

**问题**: SPIRV-Cross `vulkan-sdk-1.4.304.0` 在 iOS arm64 上 `CompilerMSL::compile()` SIGSEGV。
**解决**: 升级到 `main` 分支 + 独立编译单元 `SpirvToMSL.cpp`（隔离 ARC）+ 大栈线程辅助。
**后续**: 完全移除 SPIRV-Cross，改用 Slang 离线编译 MSL。

### 8. iOS malloc 错误 ("pointer being freed was not allocated")

**问题**: iOS 上 ShaderBundle 加载时崩溃。
**根因**: SPIRV-Cross 旧版本在 iOS arm64 上的 CompilerMSL 内存破坏 bug。
**解决**: 升级 SPIRV-Cross 到最新 main 分支，后来完全移除。

### 9. Slang Metal target 不支持 discard 和 fwidth

**问题**: Grid 着色器使用 `discard` 和 `fwidth`，Slang 的 Metal target 不支持。
**解决**:
- ShaderCompiler 编译 Metal 时传入 `__METAL__` 宏
- Grid.slang 中 `discard` 改为 `alpha=0 + early return`（`#if __METAL__`）
- `fwidth` 改为 `abs(ddx(v)) + abs(ddy(v))`（等价定义）

### 10. Slang MSL 的 buffer/texture index 映射

**问题**: Slang 扁平化分配 Metal buffer/texture index，与 Vulkan 的 (set, binding) 不直接对应。
**解决**: `parseMSLMetadata()` 从 MSL 文本中解析实际分配的 index，`buildUBOMapping`/`buildTextureMapping` 在 pipeline 创建时重建 `(set, binding) → metal index` 映射。

### 11. iOS 截图后黑屏（未完全修复）

**问题**: UIAutomation 截图命令在 iOS 上执行后，渲染恢复失败导致黑屏。
**原因**: `endSingleTimeCommands` 的 `vkQueueSubmit` + `vkQueueWaitIdle` 破坏了 Metal 帧同步状态。
**缓解**: single-time command 不覆盖 `s_lastCommittedBuffer`，命令文件执行后双重清除（remove + truncate）。
**状态**: macOS 上截图正常，iOS 上截图后黑屏问题待修复。

### 12. 全屏着色器 vertex descriptor 错误

**问题**: Sky/Shadow/IBL/后处理着色器使用 `[[vertex_id]]` 而非 `[[stage_in]]`，设置 vertex descriptor 导致 "attribute missing" 错误。
**解决**: 检测 `hasVertexInputs == false` 时跳过 vertex descriptor 设置。

## 验证结果

### macOS Metal (Apple M4 Pro)
- 编辑器 + 独立运行时正常运行
- 场景渲染完全对等 Vulkan 后端（Grid、PBR 光照、阴影、天空、后处理）
- 截图功能正常

### iOS 真机 (Apple A16 GPU, iPhone)
- 编译、签名、安装、启动全部成功
- 场景渲染正确，触摸交互正常
- 截图功能可用（截图后黑屏问题待修复）

## 提交历史

1. `b87a656` — Metal 渲染后端 + iOS 真机移植（28 文件，+5004 行）
2. `e2c7bbf` — 修复截图功能（R16G16B16A16_SFLOAT 格式支持）
3. `1b20929` — iOS UIAutomation 截图路径适配
4. `302e389` — 修复 iOS 截图功能验证通过
5. `cb34bdf` — 防止截图命令残留导致黑屏
6. `996b6f0` — iOS 截图后黑屏问题缓解（wip）
7. `cbc8772` — 移除 SPIRV-Cross，改用 Slang 离线编译 MSL
8. `066285c` — Grid 着色器 MSL 编译 + 完善 Slang MSL 绑定映射
9. `14ee948` — 重新编译全部 shaderbundle（含 default_msl 变体）
