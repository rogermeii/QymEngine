# Metal 后端实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标:** 为 QymEngine 添加原生 Metal 渲染后端，使 macOS 不再依赖 MoltenVK

**架构:** 遵循现有 VkDispatch 模式，在 `engine/renderer/metal/` 下创建 `VkMetalHandles.h` + `VkMetal.mm`，实现全部 91 个 Vulkan API 函数指针的 Metal 翻译。着色器通过 Slang 离线编译为 MSL 文本存入 shaderbundle `"default_msl"` 变体。

**技术栈:** Metal API (Objective-C++)、SDL2 Metal 支持、Slang (SLANG_METAL 目标)

---

## 文件结构

| 操作 | 文件路径 | 用途 |
|------|---------|------|
| 创建 | `engine/renderer/metal/VkMetalHandles.h` | MTL_ 前缀句柄结构体定义 |
| 创建 | `engine/renderer/metal/VkMetal.mm` | 全部 91 个 Vulkan API 的 Metal 实现 |
| 修改 | `engine/core/Window.h:9` | RenderBackend 枚举加 Metal |
| 修改 | `engine/core/Window.cpp:31-33` | Metal 后端使用 SDL_WINDOW_METAL |
| 修改 | `engine/renderer/VkDispatch.h:27,36-51` | 加 vkIsMetalBackend() 声明 |
| 修改 | `engine/renderer/VkDispatch.cpp:376-417,432-455` | backendType=5 分发 + 查询函数 |
| 修改 | `engine/CMakeLists.txt` | Obj-C++ 编译 + Metal framework 链接 |
| 修改 | `engine/asset/AssetManager.cpp:375` | 变体选择加 Metal 分支 |
| 修改 | `editor/main.cpp:11-29` | 加 --metal 命令行参数 |
| 修改 | `editor/ImGuiLayer.cpp:80` | Metal 后端用 SDL Metal init |
| 修改 | `QymEngine/main.cpp:18` | standalone 运行时支持 Metal |
| 修改 | `tools/shader_compiler/main.cpp:20-22,578-590` | 加 g_emitMsl + SLANG_METAL 变体 |

---

### Task 1: 构建基础设施 — 枚举、窗口、VkDispatch 集成

**文件:**
- 修改: `engine/core/Window.h:9`
- 修改: `engine/core/Window.cpp:14-33`
- 修改: `engine/renderer/VkDispatch.h:27,36-51`
- 修改: `engine/renderer/VkDispatch.cpp:376-417,432-455`
- 修改: `engine/CMakeLists.txt`
- 修改: `editor/main.cpp:11-29`
- 修改: `QymEngine/main.cpp`

- [ ] **步骤 1: Window.h — RenderBackend 枚举加 Metal**

`engine/core/Window.h:9`：
```cpp
enum class RenderBackend { Vulkan, D3D12, D3D11, OpenGL, GLES, Metal };
```

- [ ] **步骤 2: Window.cpp — Metal 后端窗口创建**

`engine/core/Window.cpp` 在 `else if (m_backend == RenderBackend::Vulkan)` 块后加：
```cpp
    } else if (m_backend == RenderBackend::Metal) {
        flags |= SDL_WINDOW_METAL;
    }
```

`Window.cpp` 的 `getDrawableSize` 部分（约第 67 行）加 Metal 分支：
```cpp
        if (m_backend == RenderBackend::Vulkan)
            SDL_Vulkan_GetDrawableSize(m_window, &w, &h);
        else if (m_backend == RenderBackend::Metal)
            SDL_Metal_GetDrawableSize(m_window, &w, &h);
        else
            SDL_GL_GetDrawableSize(m_window, &w, &h);
```
注意需要在文件顶部加 `#include <SDL_metal.h>`。

- [ ] **步骤 3: VkDispatch.h — 加 vkIsMetalBackend() 声明**

在 `vkIsGLESBackend()` 声明后加：
```cpp
/// 查询当前是否使用 Metal 后端
bool vkIsMetalBackend();
```

- [ ] **步骤 4: VkDispatch.cpp — Metal 后端分发和查询**

在 `extern void vkLoadOpenGLDispatch();` 后加：
```cpp
// Metal 后端加载 (MTL_ 前缀结构体，macOS 原生)
#ifdef __APPLE__
extern void vkLoadMetalDispatch();
#endif
```

`vkInitDispatch()` 的 `#else` 分支（非 Windows）中，在 OpenGL 分支后加：
```cpp
    } else if (backendType == 5) {
#ifdef __APPLE__
        vkLoadMetalDispatch();
        std::cout << "[VkDispatch] Loaded Metal backend" << std::endl;
#else
        throw std::runtime_error("[VkDispatch] Metal backend is macOS-only");
#endif
    }
```

在 `vkIsGLESBackend()` 函数后加：
```cpp
bool vkIsMetalBackend()
{
    return s_backend == 5;
}
```

- [ ] **步骤 5: engine/CMakeLists.txt — Obj-C++ 编译和 Metal 链接**

在 `list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/d3d12/.*")` 后加：
```cmake
# macOS: 启用 Objective-C++ 编译 Metal 后端
if(APPLE)
    enable_language(OBJCXX)
    file(GLOB METAL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/renderer/metal/*.mm")
    target_sources(QymEngineLib PRIVATE ${METAL_SOURCES})
    set_source_files_properties(${METAL_SOURCES} PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    target_link_libraries(QymEngineLib PUBLIC "-framework Metal" "-framework QuartzCore")
endif()
```

根 `CMakeLists.txt` 加 `OBJCXX` 语言：
```cmake
project(QymEngine LANGUAGES C CXX OBJCXX)
```
注意：需要只在 APPLE 平台加 OBJCXX，否则非 macOS 构建会失败。改为：
```cmake
project(QymEngine LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()
```

- [ ] **步骤 6: editor/main.cpp — 加 --metal 命令行参数**

在 `--gles` 分支后加：
```cpp
        else if (std::strcmp(argv[i], "--metal") == 0)
            backend = QymEngine::RenderBackend::Metal;
```

backendType 计算加 Metal：
```cpp
    int backendType = (backend == QymEngine::RenderBackend::D3D12) ? 1
                    : (backend == QymEngine::RenderBackend::D3D11) ? 2
                    : (backend == QymEngine::RenderBackend::OpenGL) ? 3
                    : (backend == QymEngine::RenderBackend::GLES) ? 4
                    : (backend == QymEngine::RenderBackend::Metal) ? 5 : 0;
```

- [ ] **步骤 7: QymEngine/main.cpp — standalone 运行时 Metal 支持**

macOS 上默认使用 Metal 后端（替代 MoltenVK）：
```cpp
#ifdef __APPLE__
    StandaloneApp() : Application({"QymEngine", 1280, 720, false, RenderBackend::Metal}) {}
#else
    StandaloneApp() : Application({"QymEngine", 1280, 720}) {}
#endif
```
对应 `onInit()` 中 `vkInitDispatch(0)` 改为：
```cpp
        int backendType = (getWindow().getBackend() == QymEngine::RenderBackend::Metal) ? 5 : 0;
        QymEngine::vkInitDispatch(backendType);
```

- [ ] **步骤 8: 创建空的 Metal 后端占位文件**

创建 `engine/renderer/metal/VkMetalHandles.h`（空头文件，只有 pragma once）和 `engine/renderer/metal/VkMetal.mm`（只含空的 `vkLoadMetalDispatch()` 函数）。

- [ ] **步骤 9: 验证构建通过**

```bash
cd build3
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHADER_COMPILER=OFF
cmake --build . --config Debug -j$(sysctl -n hw.ncpu) 2>&1 | grep -E "error:|Built target"
```
预期: 所有目标构建成功，无错误。

- [ ] **步骤 10: 提交**

```bash
git add engine/core/Window.h engine/core/Window.cpp engine/renderer/VkDispatch.h engine/renderer/VkDispatch.cpp engine/CMakeLists.txt CMakeLists.txt editor/main.cpp QymEngine/main.cpp engine/renderer/metal/
git commit -m "feat(metal): 构建基础设施 — 枚举、窗口、VkDispatch 集成"
```

---

### Task 2: VkMetalHandles.h — 全部句柄结构体

**文件:**
- 创建: `engine/renderer/metal/VkMetalHandles.h`

- [ ] **步骤 1: 编写全部 MTL_ 句柄结构体**

参照 `engine/renderer/opengl/VkOpenGLHandles.h` 的结构，创建 Metal 版本。
包含所有结构体：MTL_Instance、MTL_PhysicalDevice、MTL_Device、MTL_Queue、MTL_Surface、MTL_Swapchain、MTL_Memory、MTL_Buffer、MTL_Image、MTL_ImageView、MTL_Sampler、MTL_CommandPool、MTL_CommandBuffer、MTL_RenderPass、MTL_Framebuffer、MTL_ShaderModule、MTL_PipelineLayout、MTL_Pipeline、MTL_DescriptorSetLayout、MTL_DescriptorPool、MTL_DescriptorSet、MTL_Fence、MTL_Semaphore、MTL_DebugMessenger。

Metal 对象类型用 `id<MTLDevice>`、`id<MTLTexture>` 等，需要 `#import <Metal/Metal.h>` 和 `#import <QuartzCore/CAMetalLayer.h>`。

注意：这个头文件会被 `.mm` 文件包含，所以可以使用 Objective-C 类型。

- [ ] **步骤 2: 验证构建通过**

```bash
cmake --build . --config Debug -j$(sysctl -n hw.ncpu) 2>&1 | grep "error:"
```

- [ ] **步骤 3: 提交**

```bash
git add engine/renderer/metal/VkMetalHandles.h
git commit -m "feat(metal): VkMetalHandles.h — 全部 MTL_ 句柄结构体"
```

---

### Task 3: VkMetal.mm 核心 — Instance/Device/Queue/Surface/Swapchain

**文件:**
- 修改: `engine/renderer/metal/VkMetal.mm`

这是最关键的一步。实现引擎启动到窗口显示所需的所有函数。

- [ ] **步骤 1: 文件框架和工具宏**

```objc
#import "renderer/metal/VkMetalHandles.h"
#import "renderer/VkDispatch.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <SDL.h>
#import <SDL_metal.h>
#import <iostream>
#import <cstring>
#import <algorithm>
#import <vector>

#define AS_MTL(Type, handle) reinterpret_cast<MTL_##Type*>(handle)
#define TO_VK(VkType, ptr)  reinterpret_cast<VkType>(ptr)

static constexpr uint32_t SPIRV_MAGIC = 0x07230203;
```

- [ ] **步骤 2: VkFormat ↔ MTLPixelFormat 转换函数**

```objc
static MTLPixelFormat toMTLPixelFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:     return MTLPixelFormatBGRA8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:      return MTLPixelFormatBGRA8Unorm_sRGB;
        case VK_FORMAT_R8G8B8A8_UNORM:     return MTLPixelFormatRGBA8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:      return MTLPixelFormatRGBA8Unorm_sRGB;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return MTLPixelFormatRGBA32Float;
        case VK_FORMAT_D32_SFLOAT:         return MTLPixelFormatDepth32Float;
        case VK_FORMAT_D24_UNORM_S8_UINT:  return MTLPixelFormatDepth32Float_Stencil8;
        case VK_FORMAT_R8_UNORM:           return MTLPixelFormatR8Unorm;
        case VK_FORMAT_R16_SFLOAT:         return MTLPixelFormatR16Float;
        case VK_FORMAT_R32_SFLOAT:         return MTLPixelFormatR32Float;
        default: return MTLPixelFormatInvalid;
    }
}
```

- [ ] **步骤 3: Instance 函数 (mtl_vkCreateInstance, mtl_vkDestroyInstance, mtl_vkEnumerateInstanceExtensionProperties, mtl_vkEnumerateInstanceLayerProperties, mtl_vkGetInstanceProcAddr)**

`mtl_vkCreateInstance` 创建 `MTL_Instance`，内部创建一个 `MTL_PhysicalDevice`（对应 `MTLCreateSystemDefaultDevice()`）。

- [ ] **步骤 4: PhysicalDevice 函数 (mtl_vkEnumeratePhysicalDevices, mtl_vkGetPhysicalDeviceProperties, mtl_vkGetPhysicalDeviceFeatures, mtl_vkGetPhysicalDeviceFeatures2, mtl_vkGetPhysicalDeviceMemoryProperties, mtl_vkGetPhysicalDeviceQueueFamilyProperties, mtl_vkEnumerateDeviceExtensionProperties)**

`mtl_vkGetPhysicalDeviceProperties` 填充设备名称和限制。
`mtl_vkGetPhysicalDeviceMemoryProperties` 报告共享内存（Metal 的 managed/shared storage）。

- [ ] **步骤 5: Device/Queue 函数 (mtl_vkCreateDevice, mtl_vkDestroyDevice, mtl_vkGetDeviceQueue, mtl_vkDeviceWaitIdle)**

`mtl_vkCreateDevice` 创建 `MTL_Device`，内部持有 `id<MTLDevice>` 和 `id<MTLCommandQueue>`。

- [ ] **步骤 6: Surface 函数 (mtl_vkDestroySurfaceKHR, mtl_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, mtl_vkGetPhysicalDeviceSurfaceFormatsKHR, mtl_vkGetPhysicalDeviceSurfacePresentModesKHR, mtl_vkGetPhysicalDeviceSurfaceSupportKHR)**

Surface 通过 SDL_Metal_GetLayer() 获取 CAMetalLayer。
`mtl_vkGetPhysicalDeviceSurfaceFormatsKHR` 返回 `VK_FORMAT_B8G8R8A8_SRGB`。

注意：VulkanContext::createSurface() 调用 `SDL_Vulkan_CreateSurface()`，对 Metal 后端这会失败。需要添加一个 metal 特殊路径：Metal 后端需要自己的 surface 创建方式。参照 OpenGL 后端，surface 在 `mtl_vkGetPhysicalDeviceSurfaceCapabilitiesKHR` 中通过 `MTL_Surface` 结构体内的 `SDL_Window*` 获取。实际的 CAMetalLayer 在 swapchain 创建时获取。

- [ ] **步骤 7: Swapchain 函数 (mtl_vkCreateSwapchainKHR, mtl_vkDestroySwapchainKHR, mtl_vkGetSwapchainImagesKHR, mtl_vkAcquireNextImageKHR, mtl_vkQueuePresentKHR)**

`mtl_vkCreateSwapchainKHR`：
- 从 `MTL_Surface` 获取 `SDL_Window*`
- 调用 `SDL_Metal_CreateView()` + `SDL_Metal_GetLayer()` 获取 `CAMetalLayer*`
- 配置 layer 的 `pixelFormat`、`drawableSize`、`maximumDrawableCount`
- 创建 dummy `MTL_Image` 句柄作为 swapchain images

`mtl_vkAcquireNextImageKHR`：
- 调用 `[layer nextDrawable]` 获取 `id<CAMetalDrawable>`
- 保存 drawable 到 swapchain，更新当前 image index

`mtl_vkQueuePresentKHR`：
- 调用 `[commandBuffer presentDrawable:drawable]` + `[commandBuffer commit]`

- [ ] **步骤 8: 验证编译通过**

```bash
cmake --build . --config Debug -j$(sysctl -n hw.ncpu) 2>&1 | grep "error:"
```

- [ ] **步骤 9: 提交**

```bash
git add engine/renderer/metal/VkMetal.mm
git commit -m "feat(metal): 核心 — Instance/Device/Queue/Surface/Swapchain"
```

---

### Task 4: VkMetal.mm 资源 — Buffer/Memory/Image/ImageView/Sampler

**文件:**
- 修改: `engine/renderer/metal/VkMetal.mm`

- [ ] **步骤 1: Memory 函数 (mtl_vkAllocateMemory, mtl_vkFreeMemory, mtl_vkMapMemory, mtl_vkUnmapMemory, mtl_vkFlushMappedMemoryRanges)**

Metal 的 managed/shared 存储模式不需要显式 alloc/bind。
`MTL_Memory` 是薄封装：记录 size 和 memoryTypeIndex。
`mtl_vkMapMemory` 返回 `MTL_Buffer` 的 `[buffer contents]` 指针。

- [ ] **步骤 2: Buffer 函数 (mtl_vkCreateBuffer, mtl_vkDestroyBuffer, mtl_vkGetBufferMemoryRequirements, mtl_vkBindBufferMemory)**

`mtl_vkCreateBuffer`：创建 `[device newBufferWithLength:options:]`，使用 `MTLResourceStorageModeShared`。
`mtl_vkBindBufferMemory`：空操作（Metal buffer 创建时已分配内存）。

- [ ] **步骤 3: Image 函数 (mtl_vkCreateImage, mtl_vkDestroyImage, mtl_vkGetImageMemoryRequirements, mtl_vkBindImageMemory)**

`mtl_vkCreateImage`：创建 `MTLTextureDescriptor` → `[device newTextureWithDescriptor:]`。
根据 `VkImageCreateInfo` 的 usage/format/extent 配置 descriptor。
`mtl_vkBindImageMemory`：空操作。

- [ ] **步骤 4: ImageView 函数 (mtl_vkCreateImageView, mtl_vkDestroyImageView)**

`mtl_vkCreateImageView`：使用 `[texture newTextureViewWithPixelFormat:textureType:levels:slices:]` 创建 texture view。
对于格式相同且全 mip 范围的 view，直接引用原始 texture。

- [ ] **步骤 5: Sampler 函数 (mtl_vkCreateSampler, mtl_vkDestroySampler)**

`mtl_vkCreateSampler`：创建 `MTLSamplerDescriptor` → `[device newSamplerStateWithDescriptor:]`。
映射 VkFilter → MTLSamplerMinMagFilter，VkSamplerAddressMode → MTLSamplerAddressMode。

- [ ] **步骤 6: 验证构建通过并提交**

```bash
cmake --build . --config Debug -j$(sysctl -n hw.ncpu) 2>&1 | grep "error:"
git add engine/renderer/metal/VkMetal.mm
git commit -m "feat(metal): 资源 — Buffer/Memory/Image/ImageView/Sampler"
```

---

### Task 5: VkMetal.mm 管线 — ShaderModule/PipelineLayout/Pipeline/RenderPass/Framebuffer

**文件:**
- 修改: `engine/renderer/metal/VkMetal.mm`

- [ ] **步骤 1: ShaderModule 函数 (mtl_vkCreateShaderModule, mtl_vkDestroyShaderModule)**

检测 SPIR-V magic number：
- 如果是 SPIR-V（ImGui 着色器）：标记 `isImguiReplacement = true`
- 如果是 MSL 文本（引擎着色器）：存储到 `MTL_ShaderModule.mslSource`

- [ ] **步骤 2: 内置 ImGui MSL 着色器**

嵌入顶点和片段着色器的 MSL 源码：

```metal
// ImGui 顶点着色器
#include <metal_stdlib>
using namespace metal;

struct ImGuiUniforms {
    float2 uScale;
    float2 uTranslate;
};

struct VertexIn {
    float2 position [[attribute(0)]];
    float2 texcoord [[attribute(1)]];
    uchar4 color    [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 texcoord;
    float4 color;
};

vertex VertexOut imgui_vertex(VertexIn in [[stage_in]],
                               constant ImGuiUniforms& u [[buffer(1)]]) {
    VertexOut out;
    out.position = float4(in.position * u.uScale + u.uTranslate, 0, 1);
    out.texcoord = in.texcoord;
    out.color = float4(in.color) / 255.0;
    return out;
}

// ImGui 片段着色器
fragment float4 imgui_fragment(VertexOut in [[stage_in]],
                                texture2d<float> tex [[texture(0)]],
                                sampler smp [[sampler(0)]]) {
    return in.color * tex.sample(smp, in.texcoord);
}
```

- [ ] **步骤 3: PipelineLayout 函数 (mtl_vkCreatePipelineLayout, mtl_vkDestroyPipelineLayout)**

`MTL_PipelineLayout` 存储 set layout 引用和 push constant 范围。Metal 没有 pipeline layout 概念，这些信息在 pipeline 创建时用于计算 buffer index 映射。

- [ ] **步骤 4: RenderPass 函数 (mtl_vkCreateRenderPass, mtl_vkDestroyRenderPass)**

`MTL_RenderPass` 存储 attachment 描述（格式、load/store op）。
实际的 `MTLRenderPassDescriptor` 在 `vkCmdBeginRenderPass` 时根据 render pass + framebuffer 组合创建。

- [ ] **步骤 5: Framebuffer 函数 (mtl_vkCreateFramebuffer, mtl_vkDestroyFramebuffer)**

`MTL_Framebuffer` 存储 attachment 的 `VkImageView` 引用和尺寸。

- [ ] **步骤 6: Pipeline 函数 (mtl_vkCreateGraphicsPipelines, mtl_vkDestroyPipeline)**

这是最复杂的函数。需要：
1. 从 `MTL_ShaderModule` 获取 MSL 源码（或 ImGui 内置着色器）
2. `[device newLibraryWithSource:options:error:]` 编译 MSL
3. 获取 `MTLFunction`（顶点/片段）
4. 构建 `MTLRenderPipelineDescriptor`：
   - vertex function, fragment function
   - vertex descriptor（从 VkVertexInputStateCreateInfo 映射）
   - color attachment pixel format 和 blend state
   - depth attachment pixel format
5. `[device newRenderPipelineStateWithDescriptor:error:]`
6. 构建 `MTLDepthStencilDescriptor` → `[device newDepthStencilStateWithDescriptor:]`
7. 保存光栅化状态（cull mode、front face、polygon mode）到 `MTL_Pipeline`

VkVertexInputAttributeDescription → MTLVertexAttributeDescriptor 映射：
- format → MTLVertexFormat
- offset → offset
- binding → bufferIndex

VkPipelineColorBlendAttachmentState → MTLRenderPipelineColorAttachmentDescriptor：
- blendEnable → blendingEnabled
- srcColorBlendFactor → sourceRGBBlendFactor
- 等

- [ ] **步骤 7: 验证构建通过并提交**

```bash
cmake --build . --config Debug -j$(sysctl -n hw.ncpu) 2>&1 | grep "error:"
git add engine/renderer/metal/VkMetal.mm
git commit -m "feat(metal): 管线 — ShaderModule/Pipeline/RenderPass/Framebuffer"
```

---

### Task 6: VkMetal.mm 描述符 — DescriptorSetLayout/Pool/Set/Update

**文件:**
- 修改: `engine/renderer/metal/VkMetal.mm`

- [ ] **步骤 1: DescriptorSetLayout 函数 (mtl_vkCreateDescriptorSetLayout, mtl_vkDestroyDescriptorSetLayout)**

`MTL_DescriptorSetLayout` 存储 `std::vector<VkDescriptorSetLayoutBinding>`。

- [ ] **步骤 2: DescriptorPool 函数 (mtl_vkCreateDescriptorPool, mtl_vkDestroyDescriptorPool)**

`MTL_DescriptorPool` 是薄封装，Metal 没有 descriptor pool 概念。

- [ ] **步骤 3: DescriptorSet 函数 (mtl_vkAllocateDescriptorSets, mtl_vkFreeDescriptorSets)**

`MTL_DescriptorSet` 存储 buffer/texture/sampler 绑定数组。
分配时根据 layout 的 binding 信息初始化数组大小。

- [ ] **步骤 4: UpdateDescriptorSets (mtl_vkUpdateDescriptorSets)**

遍历 `VkWriteDescriptorSet`，按 descriptorType 更新：
- `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` → 记录 MTL buffer + offset
- `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` → 记录 MTL texture + sampler

- [ ] **步骤 5: 验证构建通过并提交**

```bash
cmake --build . --config Debug -j$(sysctl -n hw.ncpu) 2>&1 | grep "error:"
git add engine/renderer/metal/VkMetal.mm
git commit -m "feat(metal): 描述符 — DescriptorSetLayout/Pool/Set/Update"
```

---

### Task 7: VkMetal.mm 命令 — CommandPool/Buffer/录制/提交

**文件:**
- 修改: `engine/renderer/metal/VkMetal.mm`

- [ ] **步骤 1: CommandPool/Buffer 管理函数 (mtl_vkCreateCommandPool, mtl_vkDestroyCommandPool, mtl_vkAllocateCommandBuffers, mtl_vkFreeCommandBuffers, mtl_vkResetCommandPool, mtl_vkResetCommandBuffer)**

`MTL_CommandPool` 持有 device 引用。
`MTL_CommandBuffer` 在 `vkBeginCommandBuffer` 时才创建实际的 `id<MTLCommandBuffer>`。

- [ ] **步骤 2: CommandBuffer 录制开始/结束 (mtl_vkBeginCommandBuffer, mtl_vkEndCommandBuffer)**

`mtl_vkBeginCommandBuffer`：从 `MTL_Queue` 的 `id<MTLCommandQueue>` 创建 `[commandQueue commandBuffer]`。
`mtl_vkEndCommandBuffer`：标记录制完成。

- [ ] **步骤 3: RenderPass 命令 (mtl_vkCmdBeginRenderPass, mtl_vkCmdEndRenderPass)**

`mtl_vkCmdBeginRenderPass`：
1. 从 `VkRenderPassBeginInfo` 获取 render pass 和 framebuffer
2. 构建 `MTLRenderPassDescriptor`：
   - 遍历 framebuffer 的 attachments，设置 color/depth attachment 的 texture
   - 映射 VkAttachmentLoadOp → MTLLoadAction (CLEAR/LOAD/DONT_CARE)
   - 映射 VkAttachmentStoreOp → MTLStoreAction (STORE/DONT_CARE)
   - 设置 clear color/depth 值
   - 特殊处理：swapchain image 用当前 drawable 的 texture
3. `[commandBuffer renderCommandEncoderWithDescriptor:]`

`mtl_vkCmdEndRenderPass`：`[encoder endEncoding]`

- [ ] **步骤 4: 状态绑定命令 (mtl_vkCmdBindPipeline, mtl_vkCmdBindVertexBuffers, mtl_vkCmdBindIndexBuffer, mtl_vkCmdBindDescriptorSets, mtl_vkCmdPushConstants)**

`mtl_vkCmdBindPipeline`：
- `[encoder setRenderPipelineState:pipeline.pipelineState]`
- `[encoder setDepthStencilState:pipeline.depthStencilState]`
- 设置 cull mode、front facing、polygon mode

`mtl_vkCmdBindVertexBuffers`：
- `[encoder setVertexBuffer:buffer offset:offset atIndex:index]`

`mtl_vkCmdBindIndexBuffer`：
- 保存 index buffer 和 type 到 command buffer 状态

`mtl_vkCmdBindDescriptorSets`：
- 遍历 descriptor set 的绑定，调用：
  - `[encoder setVertexBuffer:...]` / `[encoder setFragmentBuffer:...]` (UBO)
  - `[encoder setFragmentTexture:...]` + `[encoder setFragmentSamplerState:...]` (texture)

Metal buffer index 映射规则：
- buffer(0) = 顶点缓冲
- buffer(1..N) = push constants + UBO（按 set/binding 计算偏移）
- texture(0..M) = 纹理绑定
- sampler(0..M) = 采样器绑定

`mtl_vkCmdPushConstants`：
- `[encoder setVertexBytes:data length:size atIndex:pushConstIndex]`
- `[encoder setFragmentBytes:data length:size atIndex:pushConstIndex]`

- [ ] **步骤 5: 绘制命令 (mtl_vkCmdDraw, mtl_vkCmdDrawIndexed)**

`mtl_vkCmdDraw`：
```objc
[encoder drawPrimitives:topology vertexStart:firstVertex vertexCount:vertexCount
                instanceCount:instanceCount baseInstance:firstInstance];
```

`mtl_vkCmdDrawIndexed`：
```objc
[encoder drawIndexedPrimitives:topology indexCount:indexCount indexType:indexType
                indexBuffer:indexBuffer indexBufferOffset:offset
                instanceCount:instanceCount baseVertex:vertexOffset baseInstance:firstInstance];
```

- [ ] **步骤 6: 视口/裁剪 (mtl_vkCmdSetViewport, mtl_vkCmdSetScissor)**

```objc
MTLViewport vp = { x, y, width, height, minDepth, maxDepth };
[encoder setViewport:vp];

MTLScissorRect sr = { x, y, width, height };
[encoder setScissorRect:sr];
```

- [ ] **步骤 7: 数据传输命令 (mtl_vkCmdCopyBuffer, mtl_vkCmdCopyBufferToImage, mtl_vkCmdCopyImageToBuffer, mtl_vkCmdPipelineBarrier, mtl_vkCmdBlitImage)**

这些命令需要 `MTLBlitCommandEncoder`：
- 如果当前有活跃的 render encoder，先 endEncoding
- 创建 blit encoder：`[commandBuffer blitCommandEncoder]`
- 执行操作后 endEncoding

`mtl_vkCmdCopyBuffer`：`[blitEncoder copyFromBuffer:... toBuffer:...]`
`mtl_vkCmdCopyBufferToImage`：`[blitEncoder copyFromBuffer:... toTexture:...]`
`mtl_vkCmdBlitImage`：`[blitEncoder copyFromTexture:... toTexture:...]`（可能需要手动 mipmap 生成）
`mtl_vkCmdPipelineBarrier`：Metal 自动处理大部分 barrier，此函数为空操作。

- [ ] **步骤 8: 提交命令 (mtl_vkQueueSubmit, mtl_vkQueueWaitIdle)**

`mtl_vkQueueSubmit`：
- 遍历 submit batch 中的 command buffers
- `[commandBuffer commit]`
- 设置 fence 的 completion handler

`mtl_vkQueueWaitIdle`：`[commandBuffer waitUntilCompleted]`

- [ ] **步骤 9: 验证构建通过并提交**

```bash
cmake --build . --config Debug -j$(sysctl -n hw.ncpu) 2>&1 | grep "error:"
git add engine/renderer/metal/VkMetal.mm
git commit -m "feat(metal): 命令 — CommandBuffer 录制与提交"
```

---

### Task 8: VkMetal.mm 同步 + vkLoadMetalDispatch()

**文件:**
- 修改: `engine/renderer/metal/VkMetal.mm`

- [ ] **步骤 1: 同步函数 (mtl_vkCreateFence, mtl_vkDestroyFence, mtl_vkWaitForFences, mtl_vkResetFences, mtl_vkCreateSemaphore, mtl_vkDestroySemaphore)**

`MTL_Fence` 使用 `dispatch_semaphore_t`：
- `mtl_vkCreateFence`：`dispatch_semaphore_create(signaled ? 1 : 0)`
- `mtl_vkWaitForFences`：`dispatch_semaphore_wait(sem, timeout)`
- `mtl_vkResetFences`：重置 semaphore

`MTL_Semaphore`：Metal 的 command buffer 提交自动排序，semaphore 为空操作。

- [ ] **步骤 2: Debug 函数 (mtl_vkCreateDebugUtilsMessengerEXT, mtl_vkDestroyDebugUtilsMessengerEXT)**

薄封装，存储回调指针。Metal 的调试信息通过 Xcode capture 或 MTLCommandBuffer error 处理。

- [ ] **步骤 3: vkLoadMetalDispatch() — 注册全部函数指针**

参照 `VkOpenGL.cpp` 的 `vkLoadOpenGLDispatch()`，用 `VK_MTL(fn)` 宏注册全部 91 个函数：
```objc
void vkLoadMetalDispatch()
{
    #define VK_MTL(fn) fn = mtl_##fn

    VK_MTL(vkCreateInstance);
    VK_MTL(vkDestroyInstance);
    // ... 全部 91 个

    #undef VK_MTL
}
```

- [ ] **步骤 4: 验证构建通过并提交**

```bash
cmake --build . --config Debug -j$(sysctl -n hw.ncpu) 2>&1 | grep "error:"
git add engine/renderer/metal/VkMetal.mm
git commit -m "feat(metal): 同步 + vkLoadMetalDispatch() 全部函数注册"
```

---

### Task 9: AssetManager/ImGuiLayer 集成

**文件:**
- 修改: `engine/asset/AssetManager.cpp:375`
- 修改: `editor/ImGuiLayer.cpp:80`

- [ ] **步骤 1: AssetManager 变体选择**

`engine/asset/AssetManager.cpp:375` 修改变体选择逻辑：
```cpp
std::string var = vkIsD3D12Backend() ? "default_dxil"
    : (vkIsD3D11Backend() ? "default_dxbc"
    : ((vkIsOpenGLBackend() || vkIsGLESBackend()) ? "default_glsl"
    : (vkIsMetalBackend() ? "default_msl"
    : "default")));
```

- [ ] **步骤 2: ImGuiLayer Metal 初始化**

`editor/ImGuiLayer.cpp:80` 附近，Metal 后端也使用 `ImGui_ImplSDL2_InitForVulkan`（因为 ImGui 的 Vulkan 后端通过 VkDispatch 桥接）。不需要特殊处理——所有 ImGui 的 vk* 调用都会走 Metal 后端的 dispatch。

但需要确认 `ImGui_ImplSDL2_InitForVulkan` 在 Metal 窗口上是否正常工作。如果不行，可能需要改用 `ImGui_ImplSDL2_InitForOther`。

- [ ] **步骤 3: 验证构建通过并提交**

```bash
cmake --build . --config Debug -j$(sysctl -n hw.ncpu) 2>&1 | grep "error:"
git add engine/asset/AssetManager.cpp editor/ImGuiLayer.cpp
git commit -m "feat(metal): AssetManager 变体选择 + ImGuiLayer 集成"
```

---

### Task 10: 着色器编译器 MSL 变体支持

**文件:**
- 修改: `tools/shader_compiler/main.cpp:20-22,578-590`

- [ ] **步骤 1: 添加 g_emitMsl 标志**

`tools/shader_compiler/main.cpp` 顶部加：
```cpp
static bool g_emitMsl = true;  // 默认编译 MSL 变体（Metal 后端用）
```

- [ ] **步骤 2: 添加 SLANG_METAL 编译分支**

在 GLSL 变体编译后（约第 590 行）加：
```cpp
    // 6. MSL 变体 (Metal 后端用)
    if (g_emitMsl) {
        std::cout << "  [msl default]" << std::endl;
        VariantResult mslDefault;
        if (compileShaderVariant(inputPath, baseName, {}, mslDefault, SLANG_METAL)) {
            mslDefault.reflectJson = variants["default"].reflectJson;
            std::cout << "  vert: " << mslDefault.vertSpv.size() << "B, frag: "
                      << mslDefault.fragSpv.size() << "B (MSL)" << std::endl;
            variants["default_msl"] = std::move(mslDefault);
        } else {
            std::cerr << "  WARNING: MSL variant failed for " << baseName << std::endl;
        }
    }
```

注意：需要确认 Slang 的 `SLANG_METAL` 目标输出是 MSL 文本。如果 Slang 用的是 `SLANG_METAL_LIB`（编译后的 metallib），则改用 `SLANG_METAL`（文本格式）。

- [ ] **步骤 3: 添加命令行选项**

命令行解析部分加：
```cpp
            else if (arg == "--no-msl")
                g_emitMsl = false;
```

打印配置加：
```cpp
    std::cout << "MSL:    " << (g_emitMsl ? "ON" : "OFF") << std::endl;
```

- [ ] **步骤 4: compileShaderVariant 处理 SLANG_METAL 目标**

在 `compileShaderVariant()` 的 target format switch 中加 SLANG_METAL 分支：
```cpp
    } else if (targetFormat == SLANG_METAL) {
        targetReq->setCodeGenTarget(SLANG_METAL);
    }
```

MSL 编译结果以文本形式存储（同 GLSL 路径）。

- [ ] **步骤 5: 验证构建通过（需要 Slang SDK，在 macOS 上是 BUILD_SHADER_COMPILER=ON 时才编译）并提交**

```bash
git add tools/shader_compiler/main.cpp
git commit -m "feat(metal): 着色器编译器 MSL 变体支持"
```

---

### Task 11: 集成测试 — 编辑器和独立运行时

**文件:** 无新文件

- [ ] **步骤 1: 重新编译着色器（如果有 Slang SDK）**

如果 Slang SDK 可用：
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHADER_COMPILER=ON
cmake --build . --target ShaderCompiler
./tools/ShaderCompiler ../assets/shaders ../assets/shaders
```

如果无 Slang SDK：暂时在 `AssetManager.cpp` 中 Metal 后端回退到 `"default"` 变体（SPIR-V），用 runtime SPIRV-Cross 或跳过此步。

- [ ] **步骤 2: 构建并运行独立运行时**

```bash
cmake --build . --config Debug -j$(sysctl -n hw.ncpu)
./engine/QymEngine
```
预期：窗口弹出，默认场景渲染正确。

- [ ] **步骤 3: 构建并运行编辑器**

```bash
./editor/QymEditor --metal
```
预期：编辑器窗口弹出，ImGui UI 正常，场景视口渲染正确。

- [ ] **步骤 4: 验证功能对等**

检查清单：
- [ ] 场景渲染（顶点/索引/纹理/光照）
- [ ] ImGui UI（菜单/面板/输入）
- [ ] 窗口调整大小
- [ ] 后处理效果
- [ ] IBL（环境光照）
- [ ] 模型预览面板

- [ ] **步骤 5: 最终提交**

```bash
git add -A
git commit -m "feat(metal): 集成测试通过 — Metal 后端完成"
```
