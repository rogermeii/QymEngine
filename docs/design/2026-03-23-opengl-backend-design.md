# OpenGL 4.5 后端设计

## 1. 目标

为 QymEngine 新增 OpenGL 4.5 桌面后端，沿用 VkDispatch shim 架构。引擎层代码零修改，通过 `--opengl` 命令行参数切换。后期可扩展 GLES 3.0 支持 Android。

## 2. 架构

### 2.1 Shim 模式

与 D3D11/D3D12 完全一致：91 个 Vulkan 函数指针在 `VkDispatch.cpp` 中分发到 `gl_vkXxx` 实现。

```
engine/renderer/opengl/
├── VkOpenGL.cpp        # 91 个 gl_vkXxx 函数实现
└── VkOpenGLHandles.h   # GL_ 前缀的 handle 结构体
```

后端编号：`backendType = 3`（0=Vulkan, 1=D3D12, 2=D3D11, 3=OpenGL）。

### 2.2 ODR 安全

Handle 结构体使用 `GL_` 前缀（`GL_Device`, `GL_Buffer`, `GL_Image` 等），通过 `reinterpret_cast` 在 `VkXxx` 句柄和 `GL_Xxx*` 之间转换，避免与 D3D12 的 `VkXxx_T` 和 D3D11 的 `D11_Xxx` 产生 ODR 冲突。

```cpp
#define AS_GL(Type, handle) reinterpret_cast<GL_##Type*>(handle)
#define TO_VK(VkType, ptr) reinterpret_cast<VkType>(ptr)
```

## 3. 对象映射

### 3.1 Handle 结构体

| Vulkan Handle | OpenGL 结构体 | 内部对象 |
|---------------|-------------|---------|
| VkInstance | GL_Instance | GLAD 初始化状态 |
| VkPhysicalDevice | GL_PhysicalDevice | GL_RENDERER / GL_VERSION 字符串 |
| VkDevice | GL_Device | SDL_GLContext |
| VkQueue | GL_Queue | 仅存 device 引用（GL 单队列） |
| VkSwapchainKHR | GL_Swapchain | SDL 默认 framebuffer (FBO 0) + SDL_GL_SwapWindow |
| VkBuffer | GL_Buffer | GLuint (VBO/IBO/UBO, glCreateBuffers) |
| VkImage | GL_Image | GLuint (texture, glCreateTextures) |
| VkImageView | GL_ImageView | 同 texture ID + format/aspect 信息 |
| VkSampler | GL_Sampler | GLuint (glCreateSamplers) |
| VkCommandBuffer | GL_CommandBuffer | 绑定状态记录 (GL 即时执行) |
| VkPipeline | GL_Pipeline | GLuint (linked program) + VAO + rasterizer/depth/blend 状态 |
| VkDescriptorSet | GL_DescriptorSet | UBO binding points + texture unit 映射 |
| VkRenderPass | GL_RenderPass | attachment 描述 (loadOp/storeOp) |
| VkFramebuffer | GL_Framebuffer | GLuint (FBO) + attachment 列表 |
| VkFence | GL_Fence | GLsync 或简单 bool |
| VkDeviceMemory | GL_Memory | 映射指针 (glMapNamedBuffer) |

### 3.2 关键差异

| 概念 | Vulkan / D3D | OpenGL 4.5 |
|------|-------------|-----------|
| 命令录制 | Command Buffer / Deferred Context | 即时执行（无延迟录制） |
| 管线状态 | 单一 PSO / 独立状态对象 | Linked program + 独立 GL state |
| 资源绑定 | Descriptor Set / Root Signature | glBindBufferBase + glBindTextureUnit |
| 资源屏障 | 显式 Barrier | 无需（驱动管理），必要时 glMemoryBarrier |
| 内存管理 | 显式分配 | 驱动管理 (glNamedBufferStorage) |
| Swapchain | DXGI / VkSwapchain | SDL 默认 framebuffer + SwapWindow |
| 坐标系 | Y-down NDC (Vulkan) / Y-up (D3D) | Y-up NDC，纹理原点左下 |

## 4. 命令录制模型

OpenGL 是即时模式 API，没有 deferred context 或 command list。采用**即时执行**策略：

- `vkBeginCommandBuffer` — 仅标记录制状态
- `vkCmdXxx` — 直接调用对应 GL 函数
- `vkEndCommandBuffer` — 标记录制结束
- `vkQueueSubmit` — 调用 `glFlush()`，处理 pending readbacks

OpenGL 是单线程绑定的，不存在多 queue 并行问题。即时执行是最自然的映射。

`GL_CommandBuffer` 结构体仅需记录当前绑定状态：

```cpp
struct GL_CommandBuffer {
    VkDevice                device = VK_NULL_HANDLE;
    bool                    isRecording = false;
    // 当前绑定状态
    VkRenderPass            currentRenderPass = VK_NULL_HANDLE;
    VkFramebuffer           currentFramebuffer = VK_NULL_HANDLE;
    VkPipeline              currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet         boundSets[4] = {};
    bool                    stateDirty = false;
    // Push constants
    uint8_t                 pushConstantData[128] = {};
    uint32_t                pushConstantSize = 0;
    GLuint                  pushConstantUBO = 0;
    // 延迟回读列表
    std::vector<GL_PendingReadback> pendingReadbacks;
};
```

## 5. 着色器系统

### 5.1 编译流水线

Shader compiler 新增 `SLANG_GLSL` 编译目标：

- Profile: `glsl_450`
- 变体名: `"default_glsl"`
- 输出: GLSL 450 源码文本
- 反射 JSON: 复用 SPIRV 变体的反射数据
- 命令行: `--no-glsl` 可跳过

在 `tools/shader_compiler/main.cpp` 中添加编译步骤，与 DXIL/DXBC 变体平行处理。

### 5.2 运行时加载

`gl_vkCreateShaderModule`: 接收 GLSL 源码文本，通过 `glCreateShader` + `glShaderSource` + `glCompileShader` 编译。

`gl_vkCreateGraphicsPipelines`: 将 VS/PS 链接为 `glCreateProgram` + `glAttachShader` + `glLinkProgram`。同时创建 VAO、设置 rasterizer/depth/blend 状态快照。

### 5.3 ImGui 着色器

与 D3D11 方案一致：嵌入 GLSL 450 版本的 ImGui VS/PS 源码。检测 SPIR-V magic (0x07230203) 后替换为预编译的 GLSL。

```glsl
// ImGui VS (GLSL 450)
#version 450
layout(binding = 0) uniform Constants {
    vec2 uScale;
    vec2 uTranslate;
};
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;
void main() {
    gl_Position = vec4(pos * uScale + uTranslate, 0.0, 1.0);
    fragColor = color;
    fragUV = uv;
}
```

### 5.4 Slang GLSL register 映射

Slang 编译 GLSL 450 时使用 `layout(binding = N)` 语法。与 DXBC 的 register 分配问题类似，需要在 pipeline 创建时解析 GLSL 源码中的 `layout(binding = N)` 来确定实际绑定点。或者依赖 Slang 的 binding 分配与 Vulkan SPIRV 一致（按 set/binding 展开）。

策略：先假设 Slang GLSL 的 binding 与 SPIRV 反射一致，如果遇到不匹配再加解析逻辑（参考 D3D11 的 RDEF 解析经验）。

## 6. 资源绑定

### 6.1 Direct State Access (DSA)

全部使用 GL 4.5 DSA API，避免 bind-to-edit 状态污染：

- Buffer: `glCreateBuffers` / `glNamedBufferStorage` / `glMapNamedBuffer`
- Texture: `glCreateTextures` / `glTextureStorage2D` / `glTextureSubImage2D`
- Sampler: `glCreateSamplers` / `glSamplerParameteri`
- FBO: `glCreateFramebuffers` / `glNamedFramebufferTexture`

### 6.2 描述符集映射

`GL_DescriptorSet` 直接存储 buffer ID 和 texture ID：

```cpp
struct GL_DescriptorSet {
    GLuint  uboBuffers[8] = {};    // UBO binding points
    GLuint  textures[8] = {};      // texture units
    GLuint  samplers[8] = {};      // sampler objects
    uint32_t uboCount = 0;
    uint32_t textureCount = 0;
};
```

`flushGraphicsState()` 在 Draw 前提交：
- `glBindBufferBase(GL_UNIFORM_BUFFER, slot, buffer)` — UBO
- `glBindTextureUnit(unit, texture)` + `glBindSampler(unit, sampler)` — 纹理
- Push constants UBO 绑定到最后一个 slot

### 6.3 资源绑定 slot 分配

与 D3D11 相同的策略：遍历所有 set layout，按 (set, binding) 顺序为 UBO 和纹理分别分配递增 slot。如果 Slang GLSL 的 binding 出现空洞（类似 D3D11 的 RDEF 问题），在 pipeline 创建时解析 GLSL 源码中的 `layout(binding = N)` 获取实际 binding。

## 7. 坐标系

OpenGL NDC 为 Y-up [-1,1]，纹理原点在左下。与引擎的 Vulkan（Y-down）和 D3D（Y-up）都不同。

处理策略：
- Camera projection: 不需要 Y-flip（GL 原生 Y-up，与 D3D 一致）
- `vkIsDirectXBackend()` 不覆盖 OpenGL。新增 `vkIsOpenGLBackend()` 用于 GL 特定的坐标调整
- ImGui VS: 不需要 `pos.y = -pos.y`（GL 的 clip space Y 与 ImGui 期望一致）
- Framebuffer blit / screenshot readback: `glReadPixels` 返回的像素是从左下开始的，需要垂直翻转

## 8. Swapchain

OpenGL 没有 swapchain 概念。通过 SDL 的默认 framebuffer (FBO 0) 模拟：

- `vkCreateSwapchainKHR`: 记录窗口尺寸，返回 GL_Swapchain（内部无实际 GL 对象）
- `vkGetSwapchainImagesKHR`: 返回包装了 FBO 0 的 GL_Image（`ownsResource = false`）
- `vkAcquireNextImageKHR`: 循环返回 imageIndex（GL 不需要 acquire）
- `vkQueuePresentKHR`: 调用 `SDL_GL_SwapWindow()`

## 9. SDL2 窗口创建

`Window.cpp` 根据 backend 设置窗口标志和 GL 属性：

```cpp
if (backend == RenderBackend::OpenGL) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    windowFlags |= SDL_WINDOW_OPENGL;
} else {
    windowFlags |= SDL_WINDOW_VULKAN;
}
```

### OpenGL 函数加载

使用 GLAD（单文件 glad.c + glad.h，直接嵌入 3rd-party/glad/）。在 `gl_vkCreateDevice` 中 SDL 创建 GL context 后调用 `gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)`。

## 10. GPU → CPU 回读

与 D3D11 类似的延迟回读方案：

- `gl_vkCmdCopyImageToBuffer`: 记录 pending readback（源 texture + 目标 buffer）
- `gl_vkQueueSubmit` 后处理: `glGetTextureImage` 或 `glReadPixels` 读取像素数据到 buffer 的 mapped memory

但因为 OpenGL 是即时模式，可以在 `CopyImageToBuffer` 中直接执行读取（无需延迟），简化实现。

## 11. RenderDoc 截帧

RenderDoc 原生支持 OpenGL 截帧。`StartFrameCapture(nullptr, nullptr)` 可自动识别 GL context（与 D3D11 方案一致）。

## 12. 文件修改清单

### 新建文件

| 文件 | 说明 |
|------|------|
| `engine/renderer/opengl/VkOpenGL.cpp` | 91 个 gl_vkXxx 函数 + vkLoadOpenGLDispatch() |
| `engine/renderer/opengl/VkOpenGLHandles.h` | GL_ 前缀结构体定义 |
| `3rd-party/glad/glad.h` | GLAD OpenGL 4.5 Core 头文件 |
| `3rd-party/glad/glad.c` | GLAD 实现 |

### 修改文件

| 文件 | 修改 |
|------|------|
| `engine/renderer/VkDispatch.h` | 添加 `vkIsOpenGLBackend()` 声明 |
| `engine/renderer/VkDispatch.cpp` | backendType=3 分发, `vkLoadOpenGLDispatch()` extern, `vkIsOpenGLBackend()` |
| `engine/core/Window.h` | RenderBackend 枚举加 `OpenGL` |
| `engine/core/Window.cpp` | OpenGL 窗口标志 + GL attribute 设置 |
| `engine/renderer/Renderer.cpp` | `shaderVariant()` 加 `"_glsl"` 分支 |
| `engine/CMakeLists.txt` | 编译 VkOpenGL.cpp + glad.c, 链接 opengl32.lib |
| `tools/shader_compiler/main.cpp` | SLANG_GLSL 目标 + `"default_glsl"` 变体 |
| `editor/main.cpp` | `--opengl` 参数解析 → backendType=3 |
| `editor/EditorApp.cpp` | RenderDoc device pointer 适配（已有 DirectX nullptr 逻辑，OpenGL 同理） |

## 13. 不实现的功能

- Bindless 描述符（GL 4.5 的 `GL_ARB_bindless_texture` 扩展覆盖率不足）
- Compute shader（当前引擎不使用）
- GLES 3.0（后期扩展，不在本阶段范围）
- 多线程命令录制（GL context 线程绑定）

## 14. 当前状态

自动化测试 23/25 通过。2 个已知失败项：
- RenderDoc 截帧：LoadLibrary 无法 hook 已加载的 opengl32.dll（需从 RenderDoc UI 启动）
- Shader 热重载后截图对比：timing 偶发不稳定

## 15. 实现中遇到的关键问题

### 15.1 Slang GLSL struct 字段名不一致

- **现象**: VS 和 PS link 失败 `struct fields mismatch for uniform block_FrameData_std140_0`
- **根因**: Slang 为 VS 和 PS 分别编译时，同一个 struct 的字段生成不同后缀（`data_0` vs `data_1`）
- **修复**: shader compiler 中 GLSL 后处理，将所有 `data_N` (N>0) 替换为 `data_0`
- **教训**: Slang GLSL 后端的命名不跨入口点一致，需要后处理规范化

### 15.2 Vulkan GLSL 内建变量不兼容

- **现象**: VS 编译失败 `gl_VertexIndex requires GL_KHR_vulkan_glsl`
- **根因**: Slang 输出的 GLSL 使用 Vulkan 特有变量（`gl_VertexIndex`、`gl_InstanceIndex`）和 `layout(push_constant)`
- **修复**: `fixupGLSL` 中替换 `gl_VertexIndex` → `gl_VertexID`，`gl_InstanceIndex` → `gl_InstanceID`，移除 `push_constant`

### 15.3 OpenGL depth range 不匹配

- **现象**: Grid 渲染在物体前方，深度关系错误
- **根因**: 引擎用 `GLM_FORCE_DEPTH_ZERO_TO_ONE`（[0,1] depth），但 OpenGL 默认 depth range 是 [-1,1]
- **修复**: `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` 在 Device 创建后调用
- **教训**: `glClipControl` 是 OpenGL 兼容 Vulkan/D3D 深度约定的关键 API

### 15.4 ImGui Y-flip 和 SPIR-V 替换

- **现象**: ImGui 上下颠倒
- **根因**: OpenGL NDC Y-up，ImGui/Vulkan 假设 Y-down。ImGui VS 需要 `gl_Position.y = -gl_Position.y`
- **复杂化**: 当 GL_ARB_gl_spirv 可用时，ImGui 的 Vulkan SPIR-V 被直接加载（绕过了自定义 GLSL 的 Y-flip）
- **修复**: 检测小 SPIR-V (<8KB) 为 ImGui shader，强制用自定义 GLSL 替换
- **教训**: ImGui 的 Vulkan SPIR-V 不能直接在 OpenGL 使用，必须替换为自定义 GLSL

### 15.5 ImGui font 纹理 sampler binding 不匹配

- **现象**: ImGui 文字显示为白色方块
- **根因**: ImGui PS GLSL 的 sampler 使用 `layout(binding = 1)` 但 ImGui 字体纹理绑定到 texture unit 0
- **修复**: 改为 `layout(binding = 0)`

### 15.6 Scissor rect Y 坐标未翻转

- **现象**: Project 和 Console 面板内容不显示
- **根因**: Vulkan scissor (0,0) 在左上，OpenGL `glScissor` (0,0) 在左下。未翻转导致底部面板被裁剪
- **修复**: `gl_vkCmdSetScissor` 中 `y = framebufferHeight - y - height`

### 15.7 Uniform block 全部 binding=0

- **现象**: 场景物体不显示（只有 Grid）
- **根因**: Slang GLSL 输出的 uniform block 没有 `layout(binding=N)`，全部默认 binding=0
- **修复**: pipeline link 后用 `glUniformBlockBinding` 按名称排序分配递增 binding（FrameData→0, MaterialParams→1, PushConstants→2）

### 15.8 Sampler binding 跨 set 冲突

- **现象**: 场景物体全红（shadow map 当作 albedo 使用）
- **根因**: 不同 Vulkan set 的 sampler 去掉 `set=N` 后 binding 相同（shadowMap=binding 1, albedoMap=binding 1）
- **修复**: `fixupGLSL` 中收集所有 sampler 的 (set, binding)，按 (set, binding) 排序后分配连续编号，确保与 `flushGraphicsState` 的 walk 顺序一致

### 15.9 Shadow UV Y-flip

- **现象**: 阴影位置上下颠倒
- **根因**: `glClipControl(GL_LOWER_LEFT)` 让 OpenGL 的 clip space 与 Vulkan 一致，不需要 D3D 的 shadow UV 翻转
- **修复**: `shadowParams.y = 0`（和 Vulkan 一样），不参与 `vkIsDirectXBackend()` 的翻转逻辑

### 15.10 RenderDoc OpenGL 截帧限制

- **现象**: `StartFrameCapture`/`TriggerCapture` 均无效，GetNumCaptures=0
- **根因**: `LoadLibrary("renderdoc.dll")` 在进程启动后加载，无法 hook 已通过 import table 加载的 opengl32.dll
- **状态**: 已知限制，从 RenderDoc UI 启动可正常截帧
- **教训**: RenderDoc 对 OpenGL 的运行时注入不可靠，必须在进程启动前注入

### 15.11 Scene View 纹理 UV 翻转

- **现象**: Scene View 中内容上下颠倒
- **根因**: OpenGL FBO 纹理 (0,0) 在左下，ImGui 纹理采样 (0,0) 在左上
- **修复**: `ImGui::Image` 的 UV 参数翻转为 `uv0=(0,1) uv1=(1,0)`
