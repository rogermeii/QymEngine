# OpenGL 4.5 后端实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 QymEngine 新增 OpenGL 4.5 桌面后端，通过 `--opengl` 切换，复用 VkDispatch shim 架构，通过 25/25 自动化测试。

**Architecture:** 沿用 D3D11 的 shim 模式，91 个 Vulkan 函数指针映射到 `gl_vkXxx` 实现。OpenGL 使用即时执行模型（无 command list）。着色器通过 Slang 编译为 GLSL 450 源码。ImGui 走 shim 拦截。

**Tech Stack:** OpenGL 4.5 Core, GLAD (函数加载), SDL2 (窗口/上下文), Slang (GLSL 编译)

**Spec:** `docs/design/2026-03-23-opengl-backend-design.md`

---

### Task 1: 添加 GLAD 并接入构建系统

**Files:**
- Create: `3rd-party/glad/glad.h`
- Create: `3rd-party/glad/glad.c`
- Create: `3rd-party/glad/KHR/khrplatform.h`
- Modify: `engine/CMakeLists.txt`

使用 https://glad.dav1d.de/ 生成 OpenGL 4.5 Core Profile + GL_KHR_debug 的 GLAD 文件。

- [ ] **Step 1: 生成 GLAD 文件**

从 https://glad.dav1d.de/ 下载（Profile: Core, API: gl 4.5, Extensions: GL_KHR_debug），放入 `3rd-party/glad/`。目录结构：

```
3rd-party/glad/
├── glad.h
├── glad.c
└── KHR/
    └── khrplatform.h
```

- [ ] **Step 2: 修改 engine/CMakeLists.txt 编译 glad.c**

在 `engine/CMakeLists.txt` 的 `add_library(QymEngineLib ...)` 行前添加 GLAD 源文件，并在 include 和 link 中引用：

```cmake
# GLAD (OpenGL 4.5 函数加载)
set(GLAD_SRC "${CMAKE_SOURCE_DIR}/3rd-party/glad/glad.c")

file(GLOB_RECURSE ENGINE_SOURCES ...)
add_library(QymEngineLib STATIC ${ENGINE_SOURCES} ${GLAD_SRC})
target_include_directories(QymEngineLib PUBLIC "${CMAKE_SOURCE_DIR}/3rd-party/glad")
```

在 Windows 上还需链接 `opengl32.lib`：

```cmake
if(WIN32)
    target_link_libraries(QymEngineLib PUBLIC opengl32)
endif()
```

- [ ] **Step 3: 验证编译通过**

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug --target QymEngineLib 2>&1 | tail -5
```

Expected: 编译成功，无错误。

- [ ] **Step 4: 提交**

```bash
git add 3rd-party/glad/ engine/CMakeLists.txt
git commit -m "chore: add GLAD OpenGL 4.5 loader"
```

---

### Task 2: 注册 OpenGL 后端到 VkDispatch + 命令行参数

**Files:**
- Modify: `engine/core/Window.h:9` — RenderBackend 枚举加 OpenGL
- Modify: `engine/core/Window.cpp:12-14` — SDL 窗口标志
- Modify: `engine/renderer/VkDispatch.h:33-39` — 添加声明
- Modify: `engine/renderer/VkDispatch.cpp:152,316-344,357-370` — 后端分发
- Modify: `editor/main.cpp:11-23` — 参数解析
- Modify: `engine/renderer/Renderer.cpp:20-26` — shaderVariant

- [ ] **Step 1: Window.h 枚举加 OpenGL**

`engine/core/Window.h:9`:
```cpp
enum class RenderBackend { Vulkan, D3D12, D3D11, OpenGL };
```

- [ ] **Step 2: Window.cpp 添加 OpenGL 窗口标志**

`engine/core/Window.cpp` 修改窗口创建逻辑：

```cpp
Uint32 flags = SDL_WINDOW_RESIZABLE;
if (m_backend == RenderBackend::OpenGL) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    flags |= SDL_WINDOW_OPENGL;
} else if (m_backend == RenderBackend::Vulkan) {
    flags |= SDL_WINDOW_VULKAN;
}
```

同样在获取 drawable size 时添加 OpenGL 分支：
```cpp
if (m_backend == RenderBackend::Vulkan)
    SDL_Vulkan_GetDrawableSize(m_window, &w, &h);
else
    SDL_GL_GetDrawableSize(m_window, &w, &h);
```

- [ ] **Step 3: VkDispatch.h 添加声明**

在 `engine/renderer/VkDispatch.h` 已有声明之后添加：

```cpp
bool vkIsOpenGLBackend();
```

- [ ] **Step 4: VkDispatch.cpp 添加分发和查询**

`engine/renderer/VkDispatch.cpp`:

1. Line 152 注释更新：`// 0=Vulkan, 1=D3D12, 2=D3D11, 3=OpenGL`
2. Line 319 后添加 extern：`extern void vkLoadOpenGLDispatch();`
3. vkInitDispatch 中 D3D11 分支后添加：
   ```cpp
   } else if (backendType == 3) {
       vkLoadOpenGLDispatch();
       std::cout << "[VkDispatch] Loaded OpenGL backend" << std::endl;
   }
   ```
4. 末尾添加：
   ```cpp
   bool vkIsOpenGLBackend() { return s_backend == 3; }
   ```

- [ ] **Step 5: editor/main.cpp 添加 --opengl 参数**

`editor/main.cpp` 参数解析部分（line 14 后）：
```cpp
else if (std::strcmp(argv[i], "--opengl") == 0)
    backend = QymEngine::RenderBackend::OpenGL;
```

backendType 映射（line 22-23）更新为：
```cpp
int backendType = (backend == QymEngine::RenderBackend::D3D12) ? 1
                : (backend == QymEngine::RenderBackend::D3D11) ? 2
                : (backend == QymEngine::RenderBackend::OpenGL) ? 3 : 0;
```

- [ ] **Step 6: Renderer.cpp shaderVariant 添加 GLSL**

`engine/renderer/Renderer.cpp:20-26`:
```cpp
static std::string shaderVariant(const std::string& base) {
    if (vkIsD3D12Backend())
        return base + "_dxil";
    if (vkIsD3D11Backend())
        return base + "_dxbc";
    if (vkIsOpenGLBackend())
        return base + "_glsl";
    return base;
}
```

- [ ] **Step 7: EditorApp.cpp RenderDoc 适配**

`editor/EditorApp.cpp` 两处 `StartFrameCapture` / `EndFrameCapture` 的条件从 `vkIsDirectXBackend()` 改为也覆盖 OpenGL：

```cpp
RENDERDOC_DevicePointer rdocDevice = (QymEngine::vkIsDirectXBackend() || QymEngine::vkIsOpenGLBackend())
    ? nullptr
    : RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(m_renderer.getContext().getInstance());
```

- [ ] **Step 8: 提交**

```bash
git add engine/core/Window.h engine/core/Window.cpp engine/renderer/VkDispatch.h engine/renderer/VkDispatch.cpp editor/main.cpp engine/renderer/Renderer.cpp editor/EditorApp.cpp
git commit -m "feat: register OpenGL backend in VkDispatch + CLI"
```

---

### Task 3: 创建 VkOpenGLHandles.h — 所有 GL_ 前缀结构体

**Files:**
- Create: `engine/renderer/opengl/VkOpenGLHandles.h`

- [ ] **Step 1: 创建完整的 handle 结构体文件**

参考 `engine/renderer/d3d11/VkD3D11Handles.h`（275 行），创建 `engine/renderer/opengl/VkOpenGLHandles.h`。所有结构体使用 `GL_` 前缀。

关键结构体列表：
- `GL_Instance` — GLAD 初始化标志
- `GL_PhysicalDevice` — GL 版本/供应商字符串
- `GL_Device` — SDL_GLContext + SDL_Window*
- `GL_Queue` — device 引用
- `GL_Surface` — HWND / SDL_Window*
- `GL_Swapchain` — 窗口尺寸 + imageCount + currentIndex
- `GL_Memory` — 映射指针 + 大小 + memoryTypeIndex
- `GL_Buffer` — GLuint buffer + size + usage + mapped pointer
- `GL_Image` — GLuint texture + format + width/height + ownsResource
- `GL_ImageView` — image 引用 + format + aspect（GL 不分离 image/view）
- `GL_Sampler` — GLuint sampler
- `GL_CommandPool` — device 引用
- `GL_CommandBuffer` — 绑定状态 + pushConstant UBO + pendingReadbacks
- `GL_RenderPass` — attachment 描述列表
- `GL_Framebuffer` — GLuint fbo + attachment 列表 + width/height
- `GL_ShaderModule` — GLSL 源码文本 + isImguiReplacement 标志
- `GL_PipelineLayout` — setLayouts + pushConstantRanges
- `GL_Pipeline` — GLuint program + GLuint vao + rasterizer/depth/blend 状态快照 + vertexStrides
- `GL_DescriptorSetLayout` — bindings 列表
- `GL_DescriptorPool` — device 引用
- `GL_DescriptorSet` — UBO GLuint[8] + texture GLuint[8] + sampler GLuint[8]
- `GL_Fence` — bool signaled
- `GL_Semaphore` — 空结构体
- `GL_DebugMessenger` — callback 指针
- `GL_PendingReadback` — 源 texture + 目标 buffer + 尺寸信息

- [ ] **Step 2: 验证编译通过**

确保头文件可以被 include 且无语法错误。

- [ ] **Step 3: 提交**

```bash
git add engine/renderer/opengl/VkOpenGLHandles.h
git commit -m "feat: add OpenGL handle structs (GL_ prefix)"
```

---

### Task 4: 创建 VkOpenGL.cpp 骨架 — 91 个 stub 函数 + vkLoadOpenGLDispatch

**Files:**
- Create: `engine/renderer/opengl/VkOpenGL.cpp`

- [ ] **Step 1: 创建文件骨架**

参考 `engine/renderer/d3d11/VkD3D11.cpp` 的 `vkLoadD3D11Dispatch()` 函数（line 3214-3260），创建 `VkOpenGL.cpp`。

文件结构：
1. `#include` 区域（VkOpenGLHandles.h, VkDispatch.h, glad.h, SDL.h 等）
2. `AS_GL` / `TO_VK` 宏定义
3. 91 个 `gl_vkXxx` stub 函数（每个打印 `[OpenGL Stub]` 并返回 VK_SUCCESS 或 nullptr）
4. `vkLoadOpenGLDispatch()` 函数：91 个 `VK_GL(fn)` 宏赋值

每个 stub 签名必须与 VkDispatch.h 中的 PFN 类型精确匹配。

- [ ] **Step 2: 验证编译链接通过**

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug --target QymEditor 2>&1 | tail -5
```

Expected: 编译链接成功。

- [ ] **Step 3: 验证 --opengl 启动不崩溃**

```bash
timeout 3 build3/editor/Debug/QymEditor.exe --opengl 2>&1 | head -5
```

Expected: 输出 `[VkDispatch] Loaded OpenGL backend`，然后可能因 stub 实现崩溃（预期内）。

- [ ] **Step 4: 提交**

```bash
git add engine/renderer/opengl/VkOpenGL.cpp
git commit -m "feat: OpenGL backend stub (91 functions)"
```

---

### Task 5: 实现 Instance / PhysicalDevice / Device / Queue 创建

**Files:**
- Modify: `engine/renderer/opengl/VkOpenGL.cpp`

实现 OpenGL 上下文的创建链路。这是最关键的一步——从 SDL 窗口创建 GL context 并初始化 GLAD。

- [ ] **Step 1: 实现 Instance 相关函数**

- `gl_vkCreateInstance`: 标记 GL_Instance 创建成功
- `gl_vkDestroyInstance`: 清理
- `gl_vkEnumerateInstanceExtensionProperties`: 返回 VK_KHR_surface + VK_KHR_win32_surface
- `gl_vkEnumerateInstanceLayerProperties`: 返回 0 层
- `gl_vkGetInstanceProcAddr`: 返回 nullptr（OpenGL 不使用）

- [ ] **Step 2: 实现 PhysicalDevice 相关函数**

- `gl_vkEnumeratePhysicalDevices`: 返回 1 个设备
- `gl_vkGetPhysicalDeviceProperties`: 从 GL_RENDERER/GL_VERSION 填充
- `gl_vkGetPhysicalDeviceFeatures` / `Features2`: 报告基本特性
- `gl_vkGetPhysicalDeviceMemoryProperties`: 返回 3 种内存类型 (DEVICE_LOCAL, HOST_VISIBLE, HOST_CACHED)
- `gl_vkGetPhysicalDeviceQueueFamilyProperties`: 返回 1 个队列族
- `gl_vkEnumerateDeviceExtensionProperties`: 返回 swapchain 扩展

- [ ] **Step 3: 实现 Device + GLAD 初始化**

- `gl_vkCreateDevice`: 从 SDL_Window 创建 GL context (`SDL_GL_CreateContext`)，然后 `gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)`。启用 `GL_KHR_debug` 回调。
- `gl_vkDestroyDevice`: `SDL_GL_DeleteContext`
- `gl_vkGetDeviceQueue`: 返回 GL_Queue
- `gl_vkDeviceWaitIdle`: `glFinish()`

**关键**: Device 需要持有 SDL_Window 指针来创建 GL context。这通过 Surface 传递。

- [ ] **Step 4: 实现 Surface 函数**

- `gl_vkCreateWin32SurfaceKHR`: 保存 HWND（从 SDL 获取）
- `gl_vkDestroySurfaceKHR`: 清理
- `gl_vkGetPhysicalDeviceSurfaceCapabilitiesKHR`: 返回窗口当前尺寸
- `gl_vkGetPhysicalDeviceSurfaceFormatsKHR`: 返回 RGBA8 SRGB
- `gl_vkGetPhysicalDeviceSurfacePresentModesKHR`: 返回 FIFO
- `gl_vkGetPhysicalDeviceSurfaceSupportKHR`: 返回 true

- [ ] **Step 5: 验证引擎初始化链路**

```bash
timeout 5 build3/editor/Debug/QymEditor.exe --opengl 2>&1 | head -20
```

Expected: 看到 `[VkOpenGL] Device created on: <GPU名称>` 和 SDL 窗口创建。

- [ ] **Step 6: 提交**

```bash
git add engine/renderer/opengl/VkOpenGL.cpp
git commit -m "feat(opengl): instance/device/queue creation with GLAD init"
```

---

### Task 6: 实现 Swapchain + Present + Synchronization

**Files:**
- Modify: `engine/renderer/opengl/VkOpenGL.cpp`

- [ ] **Step 1: 实现 Swapchain**

- `gl_vkCreateSwapchainKHR`: 记录窗口尺寸，创建 GL_Image 包装 FBO 0 的默认 framebuffer
- `gl_vkDestroySwapchainKHR`: 清理
- `gl_vkGetSwapchainImagesKHR`: 返回包装 FBO 0 的 image handles
- `gl_vkAcquireNextImageKHR`: 循环返回 imageIndex

- [ ] **Step 2: 实现 Present**

- `gl_vkQueuePresentKHR`: 调用 `SDL_GL_SwapWindow(window)`

- [ ] **Step 3: 实现 Fence / Semaphore / Submit**

- `gl_vkCreateFence` / `DestroyFence`: 简单 bool
- `gl_vkWaitForFences`: 立即返回（GL 即时模式）
- `gl_vkResetFences`: 重置 bool
- `gl_vkCreateSemaphore` / `DestroySemaphore`: 空结构体
- `gl_vkQueueSubmit`: `glFlush()` + 处理 pendingReadbacks + 设置 fence
- `gl_vkQueueWaitIdle`: `glFinish()`

- [ ] **Step 4: 实现 CommandPool / CommandBuffer**

- `gl_vkCreateCommandPool` / `DestroyCommandPool` / `ResetCommandPool`: 简单对象管理
- `gl_vkAllocateCommandBuffers`: 创建 GL_CommandBuffer + push constant UBO (`glCreateBuffers`)
- `gl_vkFreeCommandBuffers`: 销毁
- `gl_vkBeginCommandBuffer`: 重置绑定状态
- `gl_vkEndCommandBuffer`: 无操作（GL 即时执行）
- `gl_vkResetCommandBuffer`: 重置状态

- [ ] **Step 5: 验证帧循环**

```bash
timeout 5 build3/editor/Debug/QymEditor.exe --opengl 2>&1 | grep -i "init\|swap\|present\|error"
```

Expected: 看到 swapchain 创建成功，帧循环运行（窗口出现但内容为空）。

- [ ] **Step 6: 提交**

```bash
git commit -m "feat(opengl): swapchain + present + sync primitives"
```

---

### Task 7: 实现 Buffer / Memory / Image / ImageView / Sampler

**Files:**
- Modify: `engine/renderer/opengl/VkOpenGL.cpp`

- [ ] **Step 1: 实现 Memory 管理**

- `gl_vkAllocateMemory`: 记录大小和内存类型
- `gl_vkFreeMemory`: 清理 mapped pointer
- `gl_vkMapMemory`: 如果有绑定的 buffer 用 `glMapNamedBuffer`，否则分配 shadow buffer
- `gl_vkUnmapMemory`: 保持映射有效（persistent mapping 模式）
- `gl_vkFlushMappedMemoryRanges`: 无操作

- [ ] **Step 2: 实现 Buffer**

- `gl_vkCreateBuffer`: 记录 size/usage
- `gl_vkDestroyBuffer`: `glDeleteBuffers`
- `gl_vkGetBufferMemoryRequirements`: 返回对齐后的大小
- `gl_vkBindBufferMemory`: `glCreateBuffers` + `glNamedBufferStorage`（根据 memoryType 设置 flags：DYNAMIC_STORAGE/MAP_READ/MAP_WRITE）

- [ ] **Step 3: 实现 Image**

- `gl_vkCreateImage`: 记录 format/width/height/usage
- `gl_vkDestroyImage`: `glDeleteTextures`（如果 ownsResource）
- `gl_vkGetImageMemoryRequirements`: 返回大小
- `gl_vkBindImageMemory`: `glCreateTextures(GL_TEXTURE_2D)` + `glTextureStorage2D`。深度格式用 `GL_DEPTH_COMPONENT32F` 等

- [ ] **Step 4: 实现 ImageView**

- `gl_vkCreateImageView`: 记录 image/format/aspect。如果是深度 image 需要记录用途
- `gl_vkDestroyImageView`: 清理

- [ ] **Step 5: 实现 Sampler**

- `gl_vkCreateSampler`: `glCreateSamplers` + `glSamplerParameteri` 设置 filter/wrap/compare
- `gl_vkDestroySampler`: `glDeleteSamplers`

- [ ] **Step 6: 提交**

```bash
git commit -m "feat(opengl): buffer/memory/image/sampler via DSA"
```

---

### Task 8: 实现 RenderPass / Framebuffer / Pipeline / Descriptor

**Files:**
- Modify: `engine/renderer/opengl/VkOpenGL.cpp`

- [ ] **Step 1: 实现 RenderPass**

- `gl_vkCreateRenderPass`: 保存 attachment 描述（loadOp/storeOp/format）
- `gl_vkDestroyRenderPass`: 清理

- [ ] **Step 2: 实现 Framebuffer**

- `gl_vkCreateFramebuffer`: `glCreateFramebuffers` + `glNamedFramebufferTexture` 挂载 color/depth attachment
- `gl_vkDestroyFramebuffer`: `glDeleteFramebuffers`

- [ ] **Step 3: 实现 ShaderModule**

- `gl_vkCreateShaderModule`: 接收字节码，检测 SPIR-V magic → 替换为 ImGui GLSL。非 SPIR-V 数据视为 GLSL 源码文本存储
- `gl_vkDestroyShaderModule`: 清理

- [ ] **Step 4: 实现 Pipeline**

- `gl_vkCreateGraphicsPipelines`:
  1. 编译 VS/PS: `glCreateShader` + `glShaderSource` + `glCompileShader`
  2. 链接 program: `glCreateProgram` + `glAttachShader` + `glLinkProgram`
  3. 创建 VAO: `glCreateVertexArrays`，根据 vertexInput 设置属性 (`glVertexArrayAttribFormat` / `glVertexArrayAttribBinding` / `glEnableVertexArrayAttrib`)
  4. 记录 rasterizer/depth/blend 状态快照

- `gl_vkDestroyPipeline`: `glDeleteProgram` + `glDeleteVertexArrays`
- `gl_vkCreatePipelineLayout` / `DestroyPipelineLayout`: 保存 setLayouts + pushConstantRanges

- [ ] **Step 5: 实现 Descriptor**

- `gl_vkCreateDescriptorSetLayout`: 保存 bindings
- `gl_vkDestroyDescriptorSetLayout`: 清理
- `gl_vkCreateDescriptorPool` / `DestroyDescriptorPool`: 简单对象
- `gl_vkAllocateDescriptorSets`: 创建 GL_DescriptorSet
- `gl_vkFreeDescriptorSets`: 清理
- `gl_vkUpdateDescriptorSets`: 将 buffer ID / texture ID / sampler ID 存入 GL_DescriptorSet 的对应 slot

- [ ] **Step 6: 提交**

```bash
git commit -m "feat(opengl): renderpass/framebuffer/pipeline/descriptor"
```

---

### Task 9: 实现命令录制 — Draw / Bindind / RenderPass / Transfer

**Files:**
- Modify: `engine/renderer/opengl/VkOpenGL.cpp`

这是最核心的任务——实现所有 vkCmd* 函数。OpenGL 即时执行，每个 Cmd 直接调用 GL API。

- [ ] **Step 1: 实现 BeginRenderPass / EndRenderPass**

- `gl_vkCmdBeginRenderPass`:
  1. 绑定 FBO: `glBindFramebuffer(GL_FRAMEBUFFER, fbo)`（FBO 0 = 默认 swapchain）
  2. 设置 viewport: `glViewport`
  3. 设置 scissor: `glScissor` + `glEnable(GL_SCISSOR_TEST)`
  4. 根据 loadOp 执行 clear: `glClearNamedFramebufferfv` / `glClearNamedFramebufferfi`

- `gl_vkCmdEndRenderPass`:
  1. 更新 image layout 追踪
  2. `glBindFramebuffer(GL_FRAMEBUFFER, 0)` 解绑

- [ ] **Step 2: 实现 BindPipeline / BindDescriptorSets / PushConstants**

- `gl_vkCmdBindPipeline`: 记录 pipeline, `stateDirty = true`
- `gl_vkCmdBindDescriptorSets`: 记录 bound sets, `stateDirty = true`
- `gl_vkCmdPushConstants`: 写入 pushConstantData, `stateDirty = true`

- [ ] **Step 3: 实现 flushGraphicsState**

在 Draw 前统一提交 GL 状态：

```
1. glUseProgram(program)
2. glBindVertexArray(vao)
3. 设置 rasterizer state: glEnable/Disable(GL_CULL_FACE), glCullFace, glFrontFace, glPolygonMode
4. 设置 depth state: glEnable/Disable(GL_DEPTH_TEST), glDepthFunc, glDepthMask
5. 设置 blend state: glEnable/Disable(GL_BLEND), glBlendFuncSeparate, glBlendEquationSeparate
6. 绑定 UBO: glBindBufferBase(GL_UNIFORM_BUFFER, slot, buffer)
7. 绑定 texture + sampler: glBindTextureUnit(unit, tex) + glBindSampler(unit, sampler)
8. 上传 push constants: glNamedBufferSubData(pushUBO, ...) + glBindBufferBase
```

- [ ] **Step 4: 实现 syncUBOData**

与 D3D11 类似，draw 前遍历 bound descriptor sets 的 UBO，将 shadow buffer 数据通过 `glNamedBufferSubData` 上传。

- [ ] **Step 5: 实现 Draw / DrawIndexed**

- `gl_vkCmdDraw`: syncUBO + flushState + `glDrawArrays` / `glDrawArraysInstanced`
- `gl_vkCmdDrawIndexed`: syncUBO + flushState + `glDrawElements` / `glDrawElementsInstanced`

- [ ] **Step 6: 实现 BindVertexBuffers / BindIndexBuffer**

- `gl_vkCmdBindVertexBuffers`: `glVertexArrayVertexBuffer(vao, binding, buffer, offset, stride)`
- `gl_vkCmdBindIndexBuffer`: 记录 index buffer ID 和 format

- [ ] **Step 7: 实现 SetViewport / SetScissor**

- `gl_vkCmdSetViewport`: `glViewport` + `glDepthRange`
- `gl_vkCmdSetScissor`: `glScissor`

- [ ] **Step 8: 实现 Transfer 命令**

- `gl_vkCmdCopyBuffer`: `glCopyNamedBufferSubData` 或 shadow buffer memcpy
- `gl_vkCmdCopyBufferToImage`: `glTextureSubImage2D` 从 shadow buffer 上传
- `gl_vkCmdCopyImageToBuffer`: `glGetTextureImage` 或 PBO readback（即时执行可直接完成）
- `gl_vkCmdPipelineBarrier`: `glMemoryBarrier` 或无操作
- `gl_vkCmdBlitImage`: `glBlitNamedFramebuffer` 或 `glCopyImageSubData`

- [ ] **Step 9: 实现 Debug Messenger**

- `gl_vkCreateDebugUtilsMessengerEXT`: 注册 GL_KHR_debug 回调
- `gl_vkDestroyDebugUtilsMessengerEXT`: 移除回调

- [ ] **Step 10: 提交**

```bash
git commit -m "feat(opengl): command recording + draw + transfer"
```

---

### Task 10: ImGui GLSL 着色器 + 着色器系统集成

**Files:**
- Modify: `engine/renderer/opengl/VkOpenGL.cpp`

- [ ] **Step 1: 嵌入 ImGui GLSL 着色器**

在 VkOpenGL.cpp 顶部添加嵌入的 GLSL 450 源码：

```glsl
// ImGui VS
#version 450
layout(std140, binding = 0) uniform Constants {
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

// ImGui PS
#version 450
layout(binding = 1) uniform sampler2D tex;
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = fragColor * texture(tex, fragUV);
}
```

- [ ] **Step 2: 修改 gl_vkCreateShaderModule 检测 SPIR-V 并替换**

与 D3D11 相同模式：检测 SPIR-V magic (0x07230203)，根据大小判断 VS/PS，替换为预编译的 GLSL。

- [ ] **Step 3: 验证 ImGui 渲染**

```bash
timeout 8 build3/editor/Debug/QymEditor.exe --opengl 2>&1 | head -30
```

Expected: 编辑器窗口出现，ImGui 面板可见（可能没有 3D 场景内容）。

- [ ] **Step 4: 提交**

```bash
git commit -m "feat(opengl): ImGui GLSL shader + pipeline"
```

---

### Task 11: Shader Compiler 添加 GLSL 变体

**Files:**
- Modify: `tools/shader_compiler/main.cpp`

- [ ] **Step 1: 添加 g_emitGlsl 全局标志**

在 `tools/shader_compiler/main.cpp:21` 后添加：
```cpp
static bool g_emitGlsl = true;        // 默认编译 GLSL 变体（OpenGL 后端用）
```

- [ ] **Step 2: compileShaderVariant 添加 SLANG_GLSL 支持**

在 `compileShaderVariant()` 函数（line ~229）的 DXBC case 后添加：
```cpp
} else if (targetFormat == SLANG_GLSL) {
    targetDesc.profile = globalSession->findProfile("glsl_450");
}
```

- [ ] **Step 3: compileShader 添加 GLSL 编译步骤**

在 DXBC 编译段（line ~483-495）后添加：
```cpp
// 5. GLSL 变体 (OpenGL 后端用)
if (g_emitGlsl) {
    std::cout << "  [glsl default]" << std::endl;
    VariantResult glslDefault;
    if (compileShaderVariant(inputPath, baseName, {}, glslDefault, SLANG_GLSL)) {
        glslDefault.reflectJson = variants["default"].reflectJson;
        std::cout << "  vert: " << glslDefault.vertSpv.size() << "B, frag: "
                  << glslDefault.fragSpv.size() << "B (GLSL)" << std::endl;
        variants["default_glsl"] = std::move(glslDefault);
    } else {
        std::cerr << "  WARNING: GLSL default variant failed for " << baseName << std::endl;
    }
}
```

- [ ] **Step 4: 命令行参数解析**

添加 `--no-glsl` 参数解析（line ~520 后）：
```cpp
} else if (arg == "--no-glsl") {
    g_emitGlsl = false;
```

打印信息中添加 GLSL 状态。

- [ ] **Step 5: 编译 shader compiler 并重新编译着色器**

```bash
cd build3 && cmake --build . --config Debug --target ShaderCompiler
cd .. && build3/tools/Debug/ShaderCompiler.exe assets/shaders assets/shaders
```

Expected: 每个 shader 额外输出 `[glsl default]` 行，bundle 包含 `default_glsl` 变体。

- [ ] **Step 6: 验证 shader bundle 包含 GLSL 变体**

用 Python 脚本检查 .shaderbundle 文件包含 `default_glsl` 变体。

- [ ] **Step 7: 提交**

```bash
git add tools/shader_compiler/main.cpp assets/shaders/*.shaderbundle
git commit -m "feat: shader compiler GLSL 450 target (default_glsl variant)"
```

---

### Task 12: 端到端验证 + 调试 + 测试通过

**Files:**
- Modify: `engine/renderer/opengl/VkOpenGL.cpp` (调试修复)

- [ ] **Step 1: 编译全部并运行 OpenGL 后端**

```bash
cd build3 && cmake .. && cmake --build . --config Debug --target QymEditor
timeout 10 build3/editor/Debug/QymEditor.exe --opengl 2>&1 | head -50
```

- [ ] **Step 2: 截图验证场景渲染**

启动编辑器后通过 command.json 截图：
```json
{"command":"screenshot","params":{"path":"captures/opengl_test.png"}}
```

验证截图包含 3D 场景内容。

- [ ] **Step 3: 运行自动化测试**

```bash
build3/editor/Debug/QymEditor.exe --opengl &
sleep 8
python captures/run_tests.py
```

Expected: 25/25 全部通过。

- [ ] **Step 4: 调试并修复失败的测试**

根据测试输出逐个修复问题。常见问题：
- 坐标系 Y-flip
- GLSL binding slot 不匹配
- FBO attachment 类型错误
- readback 像素格式/翻转

- [ ] **Step 5: RenderDoc 截帧验证**

```bash
timeout 15 build3/editor/Debug/QymEditor.exe --opengl --capture-and-exit 2>&1 | grep -i renderdoc
```

Expected: 生成 .rdc 文件。

- [ ] **Step 6: 最终提交**

```bash
git commit -m "feat(opengl): all 25 tests passing"
```

---

### Task 13: 更新设计文档

**Files:**
- Modify: `docs/design/2026-03-23-opengl-backend-design.md`

- [ ] **Step 1: 更新文档中的"当前状态"**

将所有待完成项标记为已完成，记录实现过程中遇到的问题和解决方案（参考 D3D11 文档的§6格式）。

- [ ] **Step 2: 提交**

```bash
git add docs/design/2026-03-23-opengl-backend-design.md
git commit -m "docs: update OpenGL backend design with implementation notes"
```
