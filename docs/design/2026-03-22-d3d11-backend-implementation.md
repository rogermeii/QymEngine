# D3D11 后端设计与实现

## 1. 架构概述

D3D11 后端沿用 D3D12 的 VkD3D Shim 架构：所有 Vulkan API 通过 `VkDispatch.h` 中的函数指针分发，D3D11 后端给这些函数指针赋值为 `d3d11_vkXxx` 实现。引擎层代码无需任何修改，只需启动时 `--d3d11` 即可切换后端。

### 1.1 ODR 问题与解决方案

**核心问题**：D3D12 和 D3D11 后端都定义了 `VkXxx_T` 结构体（如 `VkDevice_T`），但成员完全不同。两个 `.cpp` 文件同时编译链接到同一个可执行文件中，违反了 C++ 的 One Definition Rule (ODR)，导致堆损坏。

**解决方案**：D3D11 的 handle 结构体使用 `D11_` 前缀命名（如 `D11_Device`, `D11_Swapchain`），通过 `reinterpret_cast` 在 `VkXxx` 句柄和 `D11_Xxx*` 之间转换：

```cpp
// VkD3D11Handles.h
struct D11_Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediateContext;
    ...
};

// VkD3D11.cpp
#define AS_D11(Type, handle) reinterpret_cast<D11_##Type*>(handle)
#define TO_VK(VkType, ptr) reinterpret_cast<VkType>(ptr)
```

### 1.2 D3D11 vs D3D12 关键差异

| 概念 | D3D12 | D3D11 |
|------|-------|-------|
| 命令录制 | Command List (显式) | Deferred Context |
| 管线状态 | 单一 PSO 对象 | 独立的 VS/PS/IL/RS/DS/BS COM 对象 |
| 资源绑定 | Root Signature + Descriptor Heap | 直接 SetConstantBuffers/SetShaderResources |
| 资源屏障 | 显式 ResourceBarrier | 无需（驱动自动管理） |
| 内存管理 | 显式 Heap (DEFAULT/UPLOAD/READBACK) | 驱动管理 (USAGE_DEFAULT/DYNAMIC/STAGING) |
| 同步 | Fence + Event | 即时模式，无需显式同步 |
| Swapchain | FLIP_DISCARD + GetBuffer(N) | DISCARD + GetBuffer(0) |

## 2. 文件结构

```
engine/renderer/d3d11/
├── VkD3D11.cpp          # 主 shim 实现 (~3350 行)
└── VkD3D11Handles.h     # D11_ 前缀的 handle 结构体定义 (~275 行)
```

其他相关修改：
- `engine/renderer/VkDispatch.cpp` — 3-way 后端分发 (0=Vulkan, 1=D3D12, 2=D3D11)
- `engine/renderer/Renderer.cpp` — `shaderVariant()` 返回 `"default_dxbc"` / `"bindless_dxbc"`
- `tools/shader_compiler/main.cpp` — 新增 `SLANG_DXBC` 编译目标
- `editor/EditorApp.cpp` — RenderDoc 截帧 device pointer 适配
- `editor/main.cpp` — `--d3d11` 命令行参数

## 3. Handle 结构体映射

| Vulkan Handle | D3D11 结构体 | 内部对象 |
|---------------|-------------|---------|
| VkInstance | D11_Instance | IDXGIFactory4 |
| VkPhysicalDevice | D11_PhysicalDevice | IDXGIAdapter1 |
| VkDevice | D11_Device | ID3D11Device + ID3D11DeviceContext (immediate) |
| VkQueue | D11_Queue | 仅存 device 引用 |
| VkSwapchainKHR | D11_Swapchain | IDXGISwapChain1 + ID3D11RenderTargetView |
| VkBuffer | D11_Buffer | ID3D11Buffer |
| VkImage | D11_Image | ID3D11Texture2D |
| VkImageView | D11_ImageView | RTV / DSV / SRV COM 对象 |
| VkCommandBuffer | D11_CommandBuffer | ID3D11DeviceContext (deferred) + ID3D11CommandList + pendingReadbacks |
| VkPipeline | D11_Pipeline | VS + PS + InputLayout + RS + DS + BS + RDEF register 映射 |
| VkDescriptorSet | D11_DescriptorSet | 直接存 ID3D11Buffer* / SRV* / SamplerState* 指针 |
| VkFence | D11_Fence | 简单 bool (D3D11 即时模式) |

## 4. 核心实现细节

### 4.1 命令缓冲 (Deferred Context)

- `vkBeginCommandBuffer`: 通过 `CreateDeferredContext()` 创建新的延迟上下文
- `vkEndCommandBuffer`: 调用 `FinishCommandList()` 生成 `ID3D11CommandList`
- `vkQueueSubmit`: 在 immediate context 上 `ExecuteCommandList()`，然后处理 pending readbacks

### 4.2 管线创建 (独立状态对象)

D3D11 没有单一 PSO 对象，管线由多个独立 COM 对象组成：
```cpp
struct D11_Pipeline {
    ComPtr<ID3D11VertexShader>      vertexShader;   // CreateVertexShader
    ComPtr<ID3D11PixelShader>       pixelShader;    // CreatePixelShader
    ComPtr<ID3D11InputLayout>       inputLayout;    // CreateInputLayout (需要 VS bytecode)
    ComPtr<ID3D11RasterizerState>   rasterizerState;
    ComPtr<ID3D11DepthStencilState> depthStencilState;
    ComPtr<ID3D11BlendState>        blendState;
    std::vector<char>               vsBytecode;     // 保留用于 CreateInputLayout
    std::vector<uint32_t>           srvRegisters;   // DXBC RDEF 中的实际 SRV register
    std::vector<uint32_t>           samplerRegisters; // DXBC RDEF 中的实际 Sampler register
};
```

Pipeline 创建时解析 PS 的 DXBC RDEF 块，提取 SRV/Sampler 的实际 register 索引。详见 §6.5。

### 4.3 资源绑定 (延迟绑定 + RDEF 驱动的 slot 分配)

- `vkCmdBindPipeline` / `vkCmdBindDescriptorSets`: 只记录状态，设置 `stateDirty = true`
- `flushGraphicsState()` 在 Draw 前统一提交到 D3D11：
  - 设置 VS/PS/InputLayout/RS/DS/BS
  - 遍历所有 bound sets 的 bindings：
    - CBV: 顺序编号 b0, b1, b2... 通过 `VSSetConstantBuffers` / `PSSetConstantBuffers`
    - SRV: 使用 RDEF 解析的 `srvRegisters[K]` 作为 register（非顺序编号）
    - Sampler: 使用 RDEF 解析的 `samplerRegisters[K]` 作为 register
  - Push constants 绑定到 CBV slot 的最后一个

### 4.4 UBO (Uniform Buffer) 策略

D3D11 不支持持久映射。采用 shadow buffer 方案：
- `vkMapMemory`: 分配 CPU 侧 shadow buffer（`calloc`），返回指针给引擎
- 引擎每帧通过 `memcpy` 写入 UBO 数据
- `syncUBOShadowBuffers()`: draw 前遍历所有 bound descriptor sets，将 shadow buffer 数据通过 `Map(WRITE_DISCARD)` + `memcpy` 上传到 D3D11 buffer

### 4.5 Push Constants

D3D11 无原生支持。使用专用 128 字节动态 constant buffer：
- 每个 CommandBuffer 创建时分配一个 `pushConstantBuffer`
- `vkCmdPushConstants`: 写入 `pushConstantData[]`
- `flushGraphicsState()`: `Map(WRITE_DISCARD)` + `memcpy` → `VSSetConstantBuffers(pushConstSlot, ...)`

### 4.6 Swapchain

使用 `DXGI_SWAP_EFFECT_DISCARD`（非 flip-model）：
- `BufferCount = 1`
- 只 `GetBuffer(0)` 获取唯一的 back buffer
- 所有 VkImage handle 指向同一个 buffer
- 引擎的多帧模型通过 imageCount 返回 3，但实际都引用同一个 buffer

### 4.7 ImGui Shader

运行时通过 `D3DCompile` 将嵌入的 HLSL 源码编译为 SM5.0 DXBC：
- VS: 与 D3D12 相同的 HLSL（包含 `pos.y = -pos.y` Y-flip）
- PS: 标准纹理采样
- 检测 SPIR-V magic (0x07230203) 后替换为预编译的 DXBC

### 4.8 Shader 编译 (Slang → DXBC)

Shader compiler 新增 `SLANG_DXBC` 编译目标：
- Profile: `sm_5_0`
- 变体名: `default_dxbc`
- ShaderBundle 二进制格式无需修改（支持任意变体名）
- 反射 JSON 复用 SPIRV 变体的反射数据（Vulkan 布局描述，非 DXBC register）

### 4.9 坐标系

与 D3D12 完全相同（D3D11 也是 Y-up NDC）：
- Camera projection: 不需要 Y-flip
- Shadow map UV: `shadowParams.y = 1` 条件翻转
- `vkIsDirectXBackend()` 统一处理 D3D11 和 D3D12

### 4.10 GPU → CPU 回读 (CopyImageToBuffer)

D3D11 deferred context 无法直接 `Map` staging texture。采用延迟回读方案：

1. `vkCmdCopyImageToBuffer` 在 deferred context 上录制 `CopySubresourceRegion`（源 texture → staging texture），同时将 staging texture 存入 `D11_CommandBuffer::pendingReadbacks`
2. `vkQueueSubmit` 中 `ExecuteCommandList()` 之后，遍历 `pendingReadbacks`：
   - 在 immediate context 上 `Map(staging texture, D3D11_MAP_READ)`
   - 逐行 `memcpy` 到目标 buffer 的 shadow memory（处理 RowPitch 对齐）
   - `Unmap` staging texture

### 4.11 RenderDoc 截帧

D3D11 后端的 VkInstance 是 shim 结构体，不是真正的 Vulkan instance。`RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE` 宏解引用会得到错误指针。

修复：DirectX 后端传 `nullptr` 给 `StartFrameCapture` / `EndFrameCapture`，让 RenderDoc 自动选择已 hook 的 D3D11 设备：
```cpp
RENDERDOC_DevicePointer rdocDevice = QymEngine::vkIsDirectXBackend()
    ? nullptr
    : RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance);
```

## 5. 当前状态

### 5.1 已完成

- [x] VkDispatch 3-way 分发 (Vulkan/D3D12/D3D11)
- [x] `--d3d11` 命令行参数
- [x] Handle 结构体定义 (D11_ 前缀，ODR 安全)
- [x] 91 个 Vulkan 函数的 D3D11 实现
- [x] Instance/PhysicalDevice/Device/Queue 创建
- [x] Swapchain 创建和 Present
- [x] Buffer/Image/ImageView/Sampler 创建
- [x] RenderPass/Framebuffer 创建
- [x] Pipeline 创建 (VS/PS/IL/RS/DS/BS) + RDEF register 解析
- [x] Descriptor Set/Pool/Layout
- [x] Command Buffer (Deferred Context)
- [x] ImGui HLSL → SM5.0 DXBC 运行时编译
- [x] Slang shader compiler DXBC 目标 (`default_dxbc` 变体)
- [x] 场景完整渲染（几何体 + 纹理 + 光照 + 阴影）
- [x] Screenshot 回读 (CopyImageToBuffer + pending readback)
- [x] RenderDoc D3D11 截帧 (nullptr device pointer)
- [x] 自动化测试套件 25/25 全部通过

### 5.2 未实现（D3D11 限制）

- Bindless 描述符（D3D11 无 descriptor indexing，`vkGetPhysicalDeviceFeatures2` 报告 unsupported）
- Compute shader
- Tessellation
- Mesh shader / Ray tracing

## 6. 遇到的关键问题

### 6.1 ODR 堆损坏

- **现象**: Swapchain 创建时 `std::vector::resize()` 触发堆损坏检测
- **原因**: D3D11 和 D3D12 的 `VkSwapchainKHR_T` 等结构体同名但 sizeof 不同，链接器/运行时使用了错误的结构体大小
- **修复**: D3D11 结构体改用 D11_ 前缀 + reinterpret_cast
- **教训**: 多后端共存时结构体必须使用唯一名称，不能依赖 translation unit 隔离

### 6.2 D3D11CreateDevice 探测导致 RenderDoc 堆损坏

- **现象**: `vkEnumeratePhysicalDevices` 中调用 `D3D11CreateDevice(adapter, ..., nullptr, nullptr, nullptr)` 探测适配器支持，RenderDoc hook 每次创建 wrapped device，导致堆损坏
- **修复**: 移除探测调用，直接使用非软件适配器
- **教训**: RenderDoc 会 hook 所有 D3D11CreateDevice 调用，即使传入的参数是用于探测的

### 6.3 Swapchain FLIP_DISCARD 限制

- **现象**: `GetBuffer(1)` 和 `GetBuffer(2)` 返回 `DXGI_ERROR_INVALID_CALL`
- **原因**: D3D11 FLIP_DISCARD 模式只允许 `GetBuffer(0)`
- **修复**: 改用 `DXGI_SWAP_EFFECT_DISCARD` + `BufferCount = 1`

### 6.4 Swapchain 重建失败

- **现象**: 窗口大小变化时 `CreateSwapChainForHwnd` 返回 `E_ACCESSDENIED`
- **原因**: D3D11 FLIP_DISCARD 不允许为同一 HWND 创建第二个 swapchain
- **修复**: 改用 `DXGI_SWAP_EFFECT_DISCARD`（同时解决了 6.3）

### 6.5 Slang DXBC register 分配与反射 JSON 不匹配

这是 D3D11 后端最隐蔽的问题，调试耗时最长。

- **现象**: Unlit shader 的 albedoMap 纹理显示异常（采样到错误纹理或黑色）
- **根因**: Slang 编译 DXBC 时按 **所有声明的资源**（包括未使用的）顺序分配 register，但引擎使用的反射 JSON 只包含**实际使用的资源**

  以 Unlit shader 为例：
  ```slang
  [[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
  [[vk::binding(1, 0)]] Sampler2D shadowMap;     // Unlit 不使用，但占 register
  [[vk::binding(0, 1)]] ConstantBuffer<MaterialParams> materialParams;
  [[vk::binding(1, 1)]] Sampler2D albedoMap;
  ```

  | 资源 | Slang DXBC 分配 | 反射 JSON | shim 按反射顺序分配 |
  |------|----------------|-----------|-------------------|
  | frame | b0 | 有 | b0 ✓ |
  | shadowMap | **t0/s0** (保留) | **无** (未使用) | — |
  | materialParams | b1 | 有 | b1 ✓ |
  | albedoMap | **t1/s1** | 有 | **t0/s0** ✗ |

- **修复**: Pipeline 创建时解析 PS DXBC 的 RDEF 块，提取每个 SRV/Sampler 的实际 register 索引。`flushGraphicsState` 绑定第 K 个纹理时使用 `pipeline->srvRegisters[K]` 而非顺序递增的 `srvSlot++`

  ```cpp
  // parseDxbcRDEF: 解析 DXBC → 提取 SRV register 列表 [t1] 和 Sampler register 列表 [s1]
  // Pipeline 创建时: parseDxbcRDEF(psData, psSize, pipeline->srvRegisters, pipeline->samplerRegisters)
  // flushGraphicsState 中:
  uint32_t actualSrv = (srvSlot < pipeline->srvRegisters.size())
      ? pipeline->srvRegisters[srvSlot] : srvSlot;
  ctx->PSSetShaderResources(actualSrv, 1, &set->srvs[bIdx]);
  ```

- **教训**: Slang 的 DXBC register 分配基于声明顺序而非使用顺序。当 SPIRV 反射（只含已使用资源）作为 DXBC 的绑定参考时，会出现 register 空洞。必须从 DXBC 二进制本身提取实际 register 映射

### 6.6 Deferred Context 无法 Map Staging Texture

- **现象**: `vkCmdCopyImageToBuffer` 的 screenshot readback 返回全零数据
- **根因**: D3D11 deferred context 的 `Map()` 只支持 `D3D11_MAP_WRITE_DISCARD`，不支持 `D3D11_MAP_READ`。在 deferred context 上 `CopySubresourceRegion` 到 staging texture 后无法立即读取
- **修复**: 引入 `D11_PendingReadback` 延迟回读机制：
  1. `CopyImageToBuffer` 在 deferred context 录制 copy，将 staging texture 存入 `cmd->pendingReadbacks`
  2. `QueueSubmit` 中 `ExecuteCommandList()` 完成 GPU 操作后，遍历 pendingReadbacks，在 immediate context 上 `Map(READ)` + `memcpy` + `Unmap`
- **教训**: D3D11 deferred context 设计上是只写的命令录制器，所有 GPU→CPU 的读操作必须在 immediate context 上完成

### 6.7 RenderDoc 设备指针不匹配

- **现象**: `StartFrameCapture` / `EndFrameCapture` 静默失败，不产生 .rdc 文件
- **根因**: `RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance)` 解引用 VkInstance 的第一个成员作为设备指针。在 D3D11 shim 中 VkInstance 是 `D11_Instance*`，解引用得到 `IDXGIFactory4*`，RenderDoc 无法识别
- **修复**: DirectX 后端传 `nullptr` 给 RenderDoc API，让其自动匹配已 hook 的 D3D11 设备
- **教训**: RenderDoc 的 `StartFrameCapture(nullptr, nullptr)` 是最可靠的通用方案

## 7. 性能特征

- **帧率**: 与 D3D12 / Vulkan 后端接近（受 Present vsync 限制）
- **命令开销**: deferred context + FinishCommandList + ExecuteCommandList 比 Vulkan 的 command buffer 模型略重
- **内存**: shadow buffer 方案对每个 UBO 需要额外的 CPU 内存副本
- **限制**: 无 bindless，每个材质需要独立的 descriptor set 绑定

## 8. 测试覆盖

自动化测试套件 25 个用例全部通过：

| 测试 | 内容 | 状态 |
|------|------|------|
| 场景加载 | JSON 反序列化，节点验证 | ✓ |
| 绘制统计 | draw calls > 0, triangles > 0 | ✓ |
| 截图 | PNG 生成 + 内容非空 | ✓ |
| 节点选择 | 层级面板点击 + 选中状态 | ✓ |
| 相机旋转 | 右键拖拽轨道旋转 | ✓ |
| 相机缩放 | 滚轮缩放 | ✓ |
| 材质系统 | .mat.json 材质分配 | ✓ |
| 快速选择 | 连续点击 4 个节点 | ✓ |
| RenderDoc 截帧 | .rdc 文件生成 | ✓ |
| Shader 热重载 | reload_shaders + 渲染恢复 | ✓ |
| 验证层 | 无 Vulkan validation error | ✓ |
