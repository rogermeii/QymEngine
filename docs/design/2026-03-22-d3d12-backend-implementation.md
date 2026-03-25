# D3D12 后端实现设计文档

**日期**: 2026-03-22
**状态**: 已实现
**范围**: VkD3D12 Shim 层、着色器双目标编译、坐标系适配

---

## 目录

1. [设计目标与架构选型](#1-设计目标与架构选型)
2. [核心映射关系](#2-核心映射关系)
3. [Root Signature 设计](#3-root-signature-设计)
4. [延迟绑定架构](#4-延迟绑定架构-deferred-state-binding)
5. [Shader 处理](#5-shader-处理)
6. [坐标系差异与处理](#6-坐标系差异与处理)
7. [遇到的关键问题与解决方案](#7-遇到的关键问题与解决方案)
8. [当前限制与后续计划](#8-当前限制与后续计划)

---

## 1. 设计目标与架构选型

### 1.1 VkD3D12 Shim 架构

QymEngine 的 D3D12 后端采用 **Vulkan API Shim** 架构：引擎所有图形调用均通过 Vulkan API 函数指针进行，D3D12 后端通过实现一套与 Vulkan 签名完全一致的 `d3d12_vkXxx` 函数，在启动时将这些函数赋值到全局函数指针表中。

核心调度机制位于 `VkDispatch.h/.cpp`：

```
VkDispatch.h  — 声明 91 个全局函数指针 (PFN_vkXxx)
VkDispatch.cpp — 定义函数指针，初始值为 nullptr
                 vkInitDispatch(useD3D12):
                   true  → 调用 vkLoadD3D12Dispatch() (定义在 VkD3D12.cpp)
                   false → LoadLibrary("vulkan-1.dll") + GetProcAddress 动态加载
```

`vkLoadD3D12Dispatch()` 函数通过宏批量赋值：

```cpp
#define VK_D3D12(fn) fn = d3d12_##fn

VK_D3D12(vkCreateInstance);
VK_D3D12(vkDestroyInstance);
// ... 共 91 个函数
```

### 1.2 为什么选择 Shim 方案

替代方案有：

- **原生 D3D12 渲染器** (如 `imgui_impl_dx12`)：需要维护两套完整的渲染器代码，工作量翻倍。
- **VKD3D-Proton**：外部运行时依赖，Linux 方案不适用于 Windows 原生 D3D12。
- **抽象图形接口层** (RHI)：需要重构整个引擎，改动过大。

Shim 方案的优势：

1. **引擎代码零改动**：所有现有的 Vulkan 调用逻辑、同步模型、descriptor 管理均可复用。
2. **增量实现**：只需实现引擎实际用到的约 91 个 Vulkan 函数，无需覆盖完整 API。
3. **ImGui 无缝集成**：ImGui 的 `imgui_impl_vulkan.cpp` 通过 `vkGetInstanceProcAddr` 加载函数，Shim 层的 `d3d12_vkGetInstanceProcAddr` 自动返回对应的 D3D12 实现。

### 1.3 VK_NO_PROTOTYPES 与动态加载

全局定义 `VK_NO_PROTOTYPES` 抑制 `vulkan.h` 中的函数原型声明，使所有 `vkXxx` 符号变为 `extern` 函数指针。这样无论 Vulkan 还是 D3D12 模式，链接时都不依赖 `vulkan-1.lib` 的导出符号。

```cpp
// VkDispatch.h
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
```

Vulkan 模式下通过 `LoadLibraryA("vulkan-1.dll")` + `GetProcAddress` 加载；D3D12 模式直接使用编译期链接的 `d3d12.lib` 和 `dxgi.lib`：

```cpp
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
```

---

## 2. 核心映射关系

### 2.1 Vulkan 概念到 D3D12 概念映射表

| Vulkan 概念 | D3D12 概念 | 实现文件中的结构体 |
|---|---|---|
| `VkInstance` | `IDXGIFactory4` | `VkInstance_T` |
| `VkPhysicalDevice` | `IDXGIAdapter1` | `VkPhysicalDevice_T` |
| `VkDevice` | `ID3D12Device` | `VkDevice_T` |
| `VkQueue` | `ID3D12CommandQueue` | `VkQueue_T` |
| `VkCommandPool` | `ID3D12CommandAllocator[]` | `VkCommandPool_T` |
| `VkCommandBuffer` | `ID3D12GraphicsCommandList` | `VkCommandBuffer_T` |
| `VkSwapchainKHR` | `IDXGISwapChain3` | `VkSwapchainKHR_T` |
| `VkSurfaceKHR` | `HWND + HINSTANCE` | `VkSurfaceKHR_T` |
| `VkRenderPass` | 无等价 (存储元数据) | `VkRenderPass_T` |
| `VkFramebuffer` | RTV/DSV handle 集合 | `VkFramebuffer_T` |
| `VkImage` | `ID3D12Resource` (Texture2D) | `VkImage_T` |
| `VkImageView` | RTV/DSV/SRV 描述符 | `VkImageView_T` |
| `VkBuffer` | `ID3D12Resource` (Buffer) | `VkBuffer_T` |
| `VkDeviceMemory` | `ID3D12Resource` (committed) | `VkDeviceMemory_T` |
| `VkPipeline` | `ID3D12PipelineState` | `VkPipeline_T` |
| `VkPipelineLayout` | `ID3D12RootSignature` | `VkPipelineLayout_T` |
| `VkShaderModule` | DXIL 字节码 | `VkShaderModule_T` |
| `VkDescriptorPool` | `ID3D12DescriptorHeap` (shader-visible) | `VkDescriptorPool_T` |
| `VkDescriptorSet` | GPU/CPU 描述符 handle + CBV 地址 | `VkDescriptorSet_T` |
| `VkFence` | `ID3D12Fence` + `HANDLE event` | `VkFence_T` |
| `VkSemaphore` | `ID3D12Fence` (简化实现) | `VkSemaphore_T` |
| `VkSampler` | Static Sampler (root sig 内) | `VkSampler_T` |
| Push Constants | Root Constants | `SetGraphicsRoot32BitConstants` |

### 2.2 Memory 模型

D3D12 的 heap 模型与 Vulkan 的 memory type 存在根本差异。Vulkan 通过 `vkGetPhysicalDeviceMemoryProperties` 报告可用的 memory type，引擎根据 `VK_MEMORY_PROPERTY_*` 标志选择 type index，然后 `vkAllocateMemory` 分配。

Shim 层向引擎报告 3 种 memory type，对应 D3D12 的 3 种 heap：

| Memory Type Index | Vulkan Property Flags | D3D12 Heap Type | 用途 |
|---|---|---|---|
| 0 | `DEVICE_LOCAL` | `D3D12_HEAP_TYPE_DEFAULT` | GPU 纹理、RT |
| 1 | `HOST_VISIBLE \| HOST_COHERENT` | `D3D12_HEAP_TYPE_UPLOAD` | 暂存/UBO |
| 2 | `HOST_VISIBLE \| HOST_COHERENT \| HOST_CACHED` | `D3D12_HEAP_TYPE_READBACK` | 截图回读 |

### 2.3 Buffer 策略

Vulkan 的 staging buffer 流程是：CPU → UPLOAD buffer → GPU CopyBuffer → DEFAULT buffer。但 D3D12 的 UPLOAD heap 可以直接被 GPU 读取 (性能有损但可用)，因此 Shim 层做了简化：

```
DEVICE_LOCAL (type 0) 的 buffer → 实际用 UPLOAD heap 创建
                                  (CPU 可写，GPU 可读，省去一次拷贝)
```

唯一例外是 **纯 `TRANSFER_DST`** 用途的 buffer (如截图 readback)，这种 buffer 需要 GPU 写入 → CPU 读取，必须用 `READBACK` heap：

```cpp
if (buffer->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT &&
    !(buffer->usage & (VERTEX | INDEX | UNIFORM))) {
    heapType = D3D12_HEAP_TYPE_READBACK;
}
```

此外，两个 UPLOAD buffer 之间的 `vkCmdCopyBuffer` 用 CPU `memcpy` 实现 (因为 UPLOAD heap 不能做 GPU copy destination)。

### 2.4 Descriptor 模型

Vulkan 的 descriptor set 模型：每个 set 有自己的 layout，pipeline layout 引用多个 set layout。绑定时可以分次绑定不同 set。

D3D12 的 root signature 模型：所有资源以 flat 的 root parameter 列表描述，没有 "set" 的概念。Shim 层的映射：

- **UBO (Uniform Buffer)** → Root CBV (`SetGraphicsRootConstantBufferView`)，按 `b0, b1, ...` 顺序编号
- **Texture (Combined Image Sampler)** → SRV Descriptor Table，所有 set 的 SRV 合并为一个连续区域
- **Sampler** → Root Signature 中的 Static Sampler (s0-s3)
- **Push Constants** → Root Constants (`SetGraphicsRoot32BitConstants`)

---

## 3. Root Signature 设计

### 3.1 从 Vulkan Pipeline Layout 自动构建

Root Signature 在 `vkCreateGraphicsPipelines` 时由 `buildRootSignature()` 函数自动构建。输入是 `VkPipelineLayout` 中的 set layout 和 push constant range。

### 3.2 布局

Root Parameter 的排列顺序：

```
[Root CBV × N]  [Root Constants]  [SRV Descriptor Table]
 b0, b1, ...     b{N} (push)       t0, t1, ... (textures)
```

具体构建逻辑：

```cpp
// 1. 统计所有 set 中的 UBO 和 SRV 总数
for (auto* setLayout : layout->setLayouts) {
    for (auto& binding : setLayout->bindings) {
        if (binding.descriptorType == UNIFORM_BUFFER) totalCbvCount++;
        else totalSrvCount += binding.descriptorCount;
    }
}

// 2. Root CBV (b0, b1, ..., b{N-1})
for (uint32_t i = 0; i < totalCbvCount; i++) {
    rootParams.push_back({ROOT_PARAMETER_TYPE_CBV, register=i, space=0});
}

// 3. Root Constants (b{N})
if (pushConstSize > 0) {
    rootParams.push_back({ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                          register=totalCbvCount, Num32BitValues=pushConstSize/4});
}

// 4. SRV Descriptor Table (t0, t1, ..., t{M-1})
if (totalSrvCount > 0) {
    srvRange = {SRV, NumDescriptors=totalSrvCount, BaseShaderRegister=0};
    rootParams.push_back({DESCRIPTOR_TABLE, ranges=[srvRange]});
}
```

### 3.3 Static Samplers

Root Signature 包含 4 个 static sampler (s0-s3)，均为 `MIN_MAG_MIP_LINEAR` + `CLAMP` 模式。这避免了运行时 sampler heap 管理的复杂性：

```cpp
D3D12_STATIC_SAMPLER_DESC samplers[4] = {};
for (int i = 0; i < 4; i++) {
    samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].ShaderRegister = i;
    samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
}
```

### 3.4 Register Space 设计

Slang 编译器生成的 DXIL 使用 flat space 0，所有 set 的资源跨 set 顺序编号。Root Signature 同样使用 `RegisterSpace = 0`，与 DXIL 的 register 编号一一对应。

例如，引擎有两个 descriptor set：
- Set 0: `b0` (FrameData UBO), `t0` (shadow map)
- Set 1: `b1` (MaterialParams UBO), `t1` (albedo), `t2` (normal)

在 Root Signature 中映射为：
```
Root Param 0: CBV b0 (FrameData)
Root Param 1: CBV b1 (MaterialParams)
Root Param 2: Root Constants b2 (Push Constants)
Root Param 3: SRV Table [t0, t1, t2]
```

---

## 4. 延迟绑定架构 (Deferred State Binding)

### 4.1 为什么需要延迟绑定

Vulkan 允许分次绑定 descriptor set，引擎代码中常见的模式：

```cpp
// 先绑定 set 0 (per-frame UBO + shadow map)
vkCmdBindDescriptorSets(..., firstSet=0, set0);
// 后绑定 set 1 (per-material 纹理)
vkCmdBindDescriptorSets(..., firstSet=1, set1);
// 最后 draw
vkCmdDrawIndexed(...);
```

但在 D3D12 中，Root Signature 的 SRV Descriptor Table 是全局的一个 root parameter，指向一段连续的 descriptor heap 区域。如果在 `BindDescriptorSets(set=0)` 时立即设置 SRV table，那么后续 `BindDescriptorSets(set=1)` 会覆盖这个 table。

### 4.2 状态记录

`VkCommandBuffer_T` 结构体中维护延迟绑定状态：

```cpp
struct VkCommandBuffer_T {
    // ... 省略其他成员 ...
    VkPipeline       currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet  boundSets[4] = {};           // 最多 4 个 set
    VkPipelineLayout boundLayout = VK_NULL_HANDLE; // 最后一次 bind 的 layout
    VkDescriptorPool boundPool = VK_NULL_HANDLE;   // descriptor pool (heap)
    bool             stateDirty = false;           // 状态变化标记
    uint32_t         srvScratchOffset = 0;         // 每帧 SRV scratch 分配偏移
};
```

`vkCmdBindPipeline` 和 `vkCmdBindDescriptorSets` 只记录状态：

```cpp
// BindPipeline: 立即设置 PSO/RootSig/Topology，标记 dirty
commandBuffer->currentPipeline = pipeline;
commandBuffer->stateDirty = true;
cmdList->SetPipelineState(pipeline->pipelineState.Get());
cmdList->SetGraphicsRootSignature(pipeline->layout->rootSignature.Get());
cmdList->IASetPrimitiveTopology(pipeline->topology);

// BindDescriptorSets: 只记录 set 引用
commandBuffer->boundSets[firstSet + i] = pDescriptorSets[i];
commandBuffer->stateDirty = true;
```

### 4.3 flushGraphicsState()

在 `vkCmdDraw` / `vkCmdDrawIndexed` 调用前，`flushGraphicsState()` 统一提交所有状态到 D3D12：

**步骤 1: 设置 Descriptor Heap**

```cpp
ID3D12DescriptorHeap* heaps[] = {pool->cbvSrvUavHeap.Get(), pool->samplerHeap.Get()};
cmdList->SetDescriptorHeaps(heapCount, heaps);
```

**步骤 2: 绑定 Root CBV**

遍历所有 set layout，按 UBO 在 set 中出现的顺序绑定到 root parameter：

```cpp
uint32_t cbvRootIdx = 0;
for (uint32_t s = 0; s < pl->setLayouts.size(); s++) {
    for (auto& binding : setLayout->bindings) {
        if (binding.descriptorType == UNIFORM_BUFFER) {
            cmdList->SetGraphicsRootConstantBufferView(
                cbvRootIdx, set->cbvAddresses[binding.binding]);
            cbvRootIdx++;
        }
    }
}
```

**步骤 3: 合并所有 set 的 SRV 到连续区域**

从 descriptor pool 的 scratch 区域分配连续空间，将所有 set 的 SRV 复制进去：

```cpp
uint32_t srvStart = pool->srvScratchBase + commandBuffer->srvScratchOffset;
uint32_t globalSrvIdx = 0;

for (每个 set, 每个非 UBO binding) {
    dev->device->CopyDescriptorsSimple(1, scratchDst, setSrc, CBV_SRV_UAV);
    globalSrvIdx++;
}

cmdList->SetGraphicsRootDescriptorTable(pl->srvTableRootIdx, dstGpuBase);
commandBuffer->srvScratchOffset += totalSrvs;
```

### 4.4 SRV Scratch 分配器

每次 draw 调用会消耗若干 scratch SRV 槽位，如果不回收，很快会溢出。解决方案：

- Descriptor Pool 创建时在 heap 尾部预留 1024 个 scratch 槽位：
  ```cpp
  pool->srvScratchBase = pool->maxCbvSrvUav;  // 紧接 set 分配区后面
  pool->srvScratchNext = pool->srvScratchBase;
  ```
- `vkBeginCommandBuffer` 时重置 scratch 偏移：
  ```cpp
  commandBuffer->srvScratchOffset = 0;
  ```
- 每帧开始时 scratch 区域自动"重置"（偏移归零，旧数据被覆盖）

---

## 5. Shader 处理

### 5.1 Slang 双目标编译

Slang 编译器 (`tools/shader_compiler/main.cpp`) 同时输出两种目标格式：

| 变体名 | 目标格式 | 用途 |
|---|---|---|
| `default` | SPIR-V | Vulkan 后端 |
| `default_dxil` | DXIL | D3D12 后端 |
| `bindless` | SPIR-V | Vulkan bindless 路径 |
| `bindless_dxil` | DXIL | D3D12 bindless 路径 |

DXIL 变体复用 SPIR-V 变体的反射 JSON（因为 binding 布局相同，只是字节码格式不同）。

### 5.2 ShaderBundle 二进制格式

所有变体打包进 `.shaderbundle` 文件。引擎根据后端选择变体：

```cpp
static std::string shaderVariant(const std::string& base) {
    if (vkIsD3D12Backend())
        return base + "_dxil";
    return base;
}
```

### 5.3 ImGui SPIR-V 自动替换为 DXIL

ImGui 的 `imgui_impl_vulkan.cpp` 内嵌了 SPIR-V 字节码，通过 `vkCreateShaderModule` 提交给驱动。D3D12 无法执行 SPIR-V，因此 Shim 层在 `vkCreateShaderModule` 中检测 SPIR-V magic number 并自动替换为预编译的 ImGui DXIL：

```cpp
// SPIR-V magic: 0x07230203
if (pCreateInfo->codeSize >= 4 && pCreateInfo->pCode[0] == 0x07230203) {
    sm->isImguiReplacement = true;
    // 较大的 shader → vertex shader, 较小的 → pixel shader
    if (pCreateInfo->codeSize > 1000) {
        sm->bytecode = imgui_vs_dxil;   // 预编译 DXIL
    } else {
        sm->bytecode = imgui_ps_dxil;
    }
}
```

预编译的 DXIL 来自 `imgui_vs.hlsl` 和 `imgui_ps.hlsl`，编译后以 C 数组形式存储在 `imgui_vs_dxil.h` / `imgui_ps_dxil.h` 中。

### 5.4 ImGui 语义名差异

ImGui DXIL shader 使用 `TEXCOORD{N}` 作为所有输入的语义名 (location 0→TEXCOORD0, 1→TEXCOORD1, 2→TEXCOORD2)，而引擎 Slang DXIL 使用标准语义 (POSITION, COLOR, TEXCOORD, NORMAL)。

PSO 创建时根据 shader 来源选择不同的语义映射：

```cpp
if (isImguiPipeline) {
    elem.SemanticName = "TEXCOORD";
    elem.SemanticIndex = attr.location;
} else {
    static const char* engineSemantics[] = {
        "POSITION", "COLOR", "TEXCOORD", "NORMAL",
        "TANGENT", "BINORMAL", "BLENDWEIGHT", "BLENDINDICES"
    };
    elem.SemanticName = engineSemantics[attr.location];
}
```

### 5.5 Shader Debug Info

Slang 编译器通过 `DebugInformation = STANDARD` 选项在 DXIL 中嵌入调试信息，便于使用 PIX 等工具调试 D3D12 shader。

---

## 6. 坐标系差异与处理

### 6.1 NDC Y 轴

| | Y 方向 | NDC 范围 |
|---|---|---|
| Vulkan | Y-down (-1=top, +1=bottom) | Z: [0, 1] |
| D3D12 | Y-up (-1=bottom, +1=top) | Z: [0, 1] |

### 6.2 Camera Projection Y-flip

GLM 的 `glm::perspective` 按 OpenGL 惯例生成投影矩阵 (Y-up)。Vulkan 需要翻转 Y：

```cpp
// Renderer.cpp — updateUniformBuffer
ubo.proj = camera->getProjMatrix(aspect, !vkIsD3D12Backend());
// getProjMatrix 内部: if (flipY) proj[1][1] *= -1;

// Vulkan: flipY = true  → proj[1][1] *= -1  (Y-down)
// D3D12:  flipY = false → 保持原始 (Y-up)
```

光源投影矩阵同理：

```cpp
glm::mat4 lightProj = glm::ortho(-15.0f, 15.0f, -15.0f, 15.0f, 0.1f, 50.0f);
if (!vkIsD3D12Backend()) lightProj[1][1] *= -1; // 仅 Vulkan 翻转
```

### 6.3 ImGui Vertex Shader Y-flip

ImGui 输出的顶点坐标基于 Vulkan NDC (Y-down)，D3D12 需要在 vertex shader 中翻转：

```hlsl
// imgui_vs.hlsl
VSOutput main(VSInput input) {
    float2 pos = input.aPos * uScale + uTranslate;
    pos.y = -pos.y;  // 翻转 Y: Vulkan NDC → D3D12 NDC
    output.Pos = float4(pos, 0, 1);
    // ...
}
```

### 6.4 Shadow Map UV Y 翻转

Vulkan 和 D3D12 的 viewport 映射方向不同：
- **Vulkan**: viewport Y 从上到下，`texV = (1 + NDC.y) / 2`
- **D3D12**: viewport Y 从下到上，`texV = (1 - NDC.y) / 2`

这导致 shadow map 采样时 UV.y 方向相反。解决方案是在 UBO 中传入后端标记：

```cpp
// Renderer.cpp
ubo.shadowParams = glm::ivec4(
    hasShadow ? 1 : 0,
    vkIsD3D12Backend() ? 1 : 0,  // y=1 表示 D3D12
    0, 0);
```

Shader 中根据标记条件翻转：

```slang
// Lit.slang — sampleShadow()
float2 shadowUV = lightNDC.xy * 0.5 + 0.5;
if (frame.shadowParams.y != 0)
    shadowUV.y = 1.0 - shadowUV.y;  // D3D12: 翻转 UV.y
```

### 6.5 Surface Format: SRGB vs UNORM

DXGI flip-model swapchain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`) 不支持 SRGB 格式创建。解决方案：用 UNORM 格式创建 swapchain，但 RTV 使用 SRGB view：

```cpp
// vkCreateSwapchainKHR 实现
DXGI_FORMAT swapChainFormat = sc->format;  // 如 DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
if (swapChainFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    swapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;  // 创建用 UNORM

// 但 RTV view 使用 SRGB
D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
rtvDesc.Format = sc->format;  // 保持 SRGB
device->CreateRenderTargetView(sc->images[i].Get(), &rtvDesc, rtvHandle);
```

Surface format 查询时同时报告 SRGB 和 UNORM 变体：

```cpp
const VkSurfaceFormatKHR formats[] = {
    {VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
```

---

## 7. 遇到的关键问题与解决方案

以下按时间顺序列出开发过程中遇到的重要 bug 及修复方案。

### 7.1 SDL_Vulkan_CreateSurface 绕过 VkDispatch

**问题**: `SDL_Vulkan_CreateSurface` 内部直接调用 Vulkan driver 的 surface 创建函数，绕过了我们的函数指针分发层。D3D12 后端的 `d3d12_vkCreateWin32SurfaceKHR` 永远不会被调用。

**修复**: D3D12 模式下改用 `SDL_GetWindowWMInfo` 获取 HWND，然后手动调用 shim 的 `vkCreateWin32SurfaceKHR`：

```cpp
void VulkanContext::createSurface() {
    if (vkIsD3D12Backend()) {
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        SDL_GetWindowWMInfo(m_window, &wm);

        VkWin32SurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        ci.hwnd = wm.info.win.window;
        ci.hinstance = GetModuleHandle(nullptr);
        vkCreateWin32SurfaceKHR(m_instance, &ci, nullptr, &m_surface);
        return;
    }
    SDL_Vulkan_CreateSurface(m_window, m_instance, &m_surface);
}
```

### 7.2 Root Signature Register Space 不匹配

**问题**: 初版实现中不同 descriptor set 使用了不同的 register space (set 0 → space 0, set 1 → space 1)，但 Slang 生成的 DXIL 将所有资源放在 flat space 0 中。绑定时 b1 找不到，材质参数全为零。

**修复**: 改为跨 set 顺序编号，所有 root parameter 使用 `RegisterSpace = 0`：

```
Set 0: b0 (UBO)     → Root Param 0 (b0, space 0)
Set 1: b1 (MatUBO)  → Root Param 1 (b1, space 0)
Push Constants       → Root Param 2 (b2, space 0)
SRV Table: t0, t1   → Root Param 3 (t0-tN, space 0)
```

### 7.3 BlendState 全零导致渲染失败

**问题**: `D3D12_BLEND` 枚举从 1 开始 (`D3D12_BLEND_ZERO = 1`)，默认值 0 是无效的。未初始化 blend state 导致 PSO 创建失败或渲染透明。

**修复**: 显式初始化默认 blend state：

```cpp
blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
```

### 7.4 ImGui SPIR-V Shader 无法在 D3D12 执行

**问题**: ImGui 内嵌 SPIR-V 字节码直接传入 `vkCreateShaderModule`，D3D12 无法执行。

**修复**: 检测 SPIR-V magic (0x07230203)，自动替换为预编译的 ImGui DXIL shader (详见第 5.3 节)。

### 7.5 nonCoherentAtomSize = 0 导致 ImGui Buffer 大小为零

**问题**: ImGui 的 `imgui_impl_vulkan.cpp` 用 `AlignBufferSize(size, nonCoherentAtomSize)` 对齐 buffer 大小。如果 `nonCoherentAtomSize = 0`，对齐运算返回 0，导致 buffer size 为 0。

**修复**: 在 `vkGetPhysicalDeviceProperties` 中设置 `nonCoherentAtomSize = 64`：

```cpp
pProperties->limits.nonCoherentAtomSize = 64; // D3D12 无此概念，但 ImGui 需要非零值
```

### 7.6 Zero-size Buffer 创建失败

**问题**: D3D12 拒绝创建 `Width = 0` 的 resource。某些引擎代码路径传入 size=0。

**修复**: 在 `vkBindBufferMemory` 中 clamp 最小大小为 256：

```cpp
VkDeviceSize bufSize = (buffer->size > 0) ? buffer->size : 256;
rd.Width = (bufSize + 255) & ~255ULL;  // 256 字节对齐
```

### 7.7 缺少 Swapchain Image Barrier

**问题**: Vulkan render pass 自动处理 attachment 的 layout 转换，但 D3D12 没有 render pass 概念。swapchain image 停留在 PRESENT 状态，直接作为 render target 导致 validation 错误。

**修复**: `vkCmdBeginRenderPass` 中自动插入 PRESENT → RENDER_TARGET barrier，`vkCmdEndRenderPass` 中根据 `finalLayout` 插入反向 barrier：

```cpp
// BeginRenderPass
if (img->currentState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    barrier.Transition.StateBefore = img->currentState;  // PRESENT
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmdList->ResourceBarrier(1, &barrier);
}

// EndRenderPass
if (finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    target = D3D12_RESOURCE_STATE_PRESENT;
else if (finalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    target = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
```

### 7.8 ImGui 字体水平镜像

**问题**: ImGui 字体渲染上下颠倒。原因是 D3D12 NDC Y-up，而 ImGui 的坐标计算假设 Vulkan NDC Y-down。

**修复**: 在 ImGui DXIL vertex shader 中加 `pos.y = -pos.y` (详见第 6.3 节)。

### 7.9 UBO (FrameData) 未正确绑定

**问题**: `vkCmdBindDescriptorSets` 绑定 set 1 时，`cbvRootBase` 没有考虑 `firstSet` 偏移。绑定 set 1 的 CBV 覆盖了 set 0 的 root CBV (b0)，导致相机矩阵丢失。

**修复**: `flushGraphicsState()` 中按 set layout 顺序遍历，逐步递增 `cbvRootIdx`：

```cpp
uint32_t cbvRootIdx = 0;
for (uint32_t s = 0; s < pl->setLayouts.size() && s < 4; s++) {
    for (auto& binding : setLayout->bindings) {
        if (binding.descriptorType == UNIFORM_BUFFER) {
            // 即使 set 未绑定也递增 index，保持偏移正确
            if (set) cmdList->SetGraphicsRootConstantBufferView(cbvRootIdx, ...);
            cbvRootIdx++;
        }
    }
}
```

### 7.10 SRV Descriptor Table 被覆盖

**问题**: 分次绑定 set 0 和 set 1 时，每次 bind 都立即设置 SRV table。set 1 的 SRV table 覆盖了 set 0 的 shadow map，导致采样结果全黑。

**修复**: 实现延迟绑定架构 (详见第 4 节)。`BindDescriptorSets` 只记录状态，`flushGraphicsState()` 在 draw 前将所有 set 的 SRV 合并到一段连续的 scratch 区域。

### 7.11 Descriptor Pool SRV 分配器溢出

**问题**: 每次 draw 分配新的 SRV scratch 槽位，永不回收。几帧后溢出 descriptor heap 容量。

**修复**: 每帧通过 `BeginCommandBuffer` 重置 `srvScratchOffset = 0`，实现隐式的帧级回收。Descriptor Pool 在 heap 尾部预留 1024 个 scratch 槽位：

```cpp
uint32_t scratchSize = 1024;
uint32_t totalHeapSize = pool->maxCbvSrvUav + scratchSize;
pool->srvScratchBase = pool->maxCbvSrvUav;
```

### 7.12 颜色偏暗 (SRGB vs UNORM)

**问题**: 引擎 offscreen RT 使用 SRGB 格式，但 D3D12 swapchain 只报告 UNORM 格式。SRGB 图像以 UNORM 方式显示时缺少 gamma 校正，颜色偏暗。

**修复**: Surface format 查询同时报告 SRGB 变体。DXGI flip-model swapchain 用 UNORM 创建但 RTV 用 SRGB view (详见第 6.5 节)。

### 7.13 Shadow Map UV Y 翻转

**问题**: Vulkan 和 D3D12 viewport 的 NDC→screen Y 映射相反，导致 shadow map 采样位置错误，阴影出现在物体的反方向。

**修复**: 通过 `shadowParams.y` 标记后端类型，shader 中条件翻转 `shadowUV.y` (详见第 6.4 节)。

### 7.14 截图 Crash — Readback Buffer 使用了 UPLOAD Heap

**问题**: 截图功能需要将 GPU 渲染结果复制回 CPU 可读 buffer。但 buffer 被分配到 UPLOAD heap (因为简化策略将 DEFAULT 映射到 UPLOAD)。UPLOAD heap 不能做 GPU 写入目标，`CopyTextureRegion` 崩溃。

**修复**: 在 `vkBindBufferMemory` 中特殊处理纯 `TRANSFER_DST` 用途的 buffer，使用 `READBACK` heap：

```cpp
if (buffer->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT &&
    !(buffer->usage & (VERTEX | INDEX | UNIFORM))) {
    heapType = D3D12_HEAP_TYPE_READBACK;
}
```

### 7.15 CopyImageToBuffer Row Pitch 溢出

**问题**: D3D12 的 `CopyTextureRegion` 要求 row pitch 256 字节对齐。但目标 buffer 按原始 pitch (如 `1280 × 4 = 5120` 字节/行) 分配，对齐后的 pitch (`5376` 字节/行) 导致总大小超出 buffer 容量。

**修复**: 创建临时对齐 buffer，GPU 侧 `CopyTextureRegion` 写入临时 buffer，再用 `CopyBufferRegion` 逐行拷贝回目标 buffer：

```cpp
if (needTempBuf) {
    // 1. CopyTextureRegion → tempBuf (对齐 pitch)
    cmdList->CopyTextureRegion(&dst_temp, 0, 0, 0, &src_image, nullptr);
    // 2. 逐行 CopyBufferRegion → dstBuffer (原始 pitch)
    for (uint32_t row = 0; row < height; row++) {
        cmdList->CopyBufferRegion(
            dstBuffer, region.bufferOffset + row * rowPitch,
            tempBuf, row * alignedRowPitch,
            rowPitch);
    }
    deferRelease(tempBuf);  // GPU 完成后释放
}
```

### 7.16 PSO 创建失败 (Unlit Shader)

**问题**: 一个使用 swapchain render pass 的 Triangle pipeline PSO 创建失败。经分析是未使用的 pipeline（引擎主渲染路径使用 offscreen pipeline），不影响实际渲染。

**处理**: 记录错误日志，不阻塞初始化流程。PSO 创建失败时 `pipelineState` 为 null，draw 调用中检查：

```cpp
if (!commandBuffer->currentPipeline || !commandBuffer->currentPipeline->pipelineState) return;
```

### 7.17 RenderDoc Vulkan 截帧不工作

**问题**: RenderDoc 的 `StartFrameCapture` 需要传入 `VkInstance` 句柄作为设备标识，但原代码传入 `nullptr`。D3D12 后端的 `VkInstance` 是 shim 层分配的结构体指针，RenderDoc 无法识别。

**修复**: `StartFrameCapture` 传入实际的 `VkInstance` 句柄。注意 D3D12 后端的 RenderDoc 需要使用 D3D12 截帧模式而非 Vulkan 模式。

---

## 8. 当前限制与后续计划

### 8.1 当前限制

1. **Bindless 路径未在 D3D12 实现**
   D3D12 天然支持 bindless (unbounded descriptor arrays)，但需要额外的 root signature 配置和 descriptor heap 管理。当前仅实现了 per-material descriptor set 路径。

2. **一个 PSO 创建失败**
   未使用的 swapchain Triangle pipeline 创建失败 (见 7.16)。不影响渲染，但日志中有错误信息。

3. **大量调试日志未清理**
   `VkD3D12.cpp` 中包含大量 `fprintf(stderr, ...)` 调试输出 (buffer 创建、vertex buffer 绑定、draw call 等)，发布时需要清理。

4. **Android 不适用**
   D3D12 仅限 Windows 平台。Android 继续使用原生 Vulkan 后端。所有 D3D12 代码通过 `#ifdef _WIN32` 隔离。

5. **Blit 不支持缩放**
   `vkCmdBlitImage` 的 D3D12 实现使用 `CopyResource` / `CopyTextureRegion`，不支持缩放 (Vulkan 支持)。同尺寸 blit 正常工作。

6. **Sampler 固定为 Static Sampler**
   所有 sampler 使用 root signature 中的 4 个 static sampler (线性插值 + clamp)。不支持运行时创建不同配置的 sampler (如 nearest/wrap)。

### 8.2 后续计划

- 清理调试日志，添加日志级别控制
- 实现 D3D12 bindless 路径 (unbounded SRV array + descriptor indexing)
- 支持 sampler heap 动态管理 (替代 static sampler)
- 修复未使用 pipeline 的 PSO 创建失败
- 性能优化：减少每帧的 `CopyDescriptorsSimple` 调用，使用 ring buffer 替代 scratch 分配
- 考虑使用 D3D12 Enhanced Barriers (ID3D12GraphicsCommandList7) 替代手动 resource state tracking

---

## 附录: 文件索引

| 文件路径 | 说明 |
|---|---|
| `engine/renderer/VkDispatch.h` | 91 个全局函数指针声明 |
| `engine/renderer/VkDispatch.cpp` | 函数指针定义 + 加载逻辑 |
| `engine/renderer/d3d12/VkD3D12.h` | D3D12 shim 入口声明 |
| `engine/renderer/d3d12/VkD3D12.cpp` | D3D12 shim 完整实现 (~3200 行) |
| `engine/renderer/d3d12/VkD3D12Handles.h` | Vulkan 句柄的 D3D12 内部结构体 |
| `engine/renderer/d3d12/imgui_vs.hlsl` | ImGui vertex shader (HLSL) |
| `engine/renderer/d3d12/imgui_ps.hlsl` | ImGui pixel shader (HLSL) |
| `engine/renderer/d3d12/imgui_vs_dxil.h` | 预编译 ImGui VS DXIL 字节码 |
| `engine/renderer/d3d12/imgui_ps_dxil.h` | 预编译 ImGui PS DXIL 字节码 |
| `tools/shader_compiler/main.cpp` | Slang → SPIR-V + DXIL 编译器 |
| `engine/renderer/Renderer.cpp` | 渲染器 (含 D3D12 坐标适配) |
| `engine/renderer/VulkanContext.cpp` | Vulkan 上下文 (含 surface 创建绕过) |
