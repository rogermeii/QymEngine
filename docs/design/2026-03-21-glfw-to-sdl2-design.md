# GLFW → SDL2 替换设计

**日期**: 2026-03-21
**目标**: 将窗口/输入层从 GLFW 替换为 SDL2，在 Windows 上保持现有功能不变，为后续 Android 移植打基础。

## 背景

当前引擎使用 GLFW 3.4 管理窗口和输入。GLFW 不支持 Android，而 SDL2 原生支持 Windows/Android/iOS/Linux/macOS，是跨平台的最佳选择。

## 改动范围

### 1. Window 层 (`engine/core/Window.h/.cpp`)

| GLFW | SDL2 |
|------|------|
| `glfwInit()` | `SDL_Init(SDL_INIT_VIDEO \| SDL_INIT_EVENTS)` |
| `glfwCreateWindow()` | `SDL_CreateWindow()` with `SDL_WINDOW_VULKAN` |
| `glfwPollEvents()` | `SDL_PollEvent()` 循环 |
| `glfwSetFramebufferSizeCallback()` | `SDL_WINDOWEVENT_RESIZED` 事件 |
| `glfwWindowShouldClose()` | SDL_QUIT 事件设置标志 |
| `glfwDestroyWindow()` / `glfwTerminate()` | `SDL_DestroyWindow()` / `SDL_Quit()` |
| `glfwGetFramebufferSize()` | `SDL_Vulkan_GetDrawableSize()` |

Window 类对外接口保持不变（`init()`, `shouldClose()`, `pollEvents()`, `getWidth/Height()`），内部实现从 GLFW 切换到 SDL2。需要新增 `processEvents()` 方法处理 SDL 事件分发。

### 2. Vulkan Surface 创建 (`engine/renderer/VulkanContext.cpp`)

| GLFW | SDL2 |
|------|------|
| `glfwGetRequiredInstanceExtensions()` | `SDL_Vulkan_GetInstanceExtensions()` |
| `glfwCreateWindowSurface()` | `SDL_Vulkan_CreateSurface()` |

VulkanContext::createSurface() 改为接收 `SDL_Window*`。

### 3. ImGui 后端 (`editor/ImGuiLayer.cpp`)

| GLFW | SDL2 |
|------|------|
| `#include "imgui_impl_glfw.h"` | `#include "imgui_impl_sdl2.h"` |
| `ImGui_ImplGlfw_InitForVulkan(window, true)` | `ImGui_ImplSDL2_InitForVulkan(window)` |
| `ImGui_ImplGlfw_NewFrame()` | `ImGui_ImplSDL2_NewFrame()` |
| `ImGui_ImplGlfw_Shutdown()` | `ImGui_ImplSDL2_Shutdown()` |

需要在事件循环中调用 `ImGui_ImplSDL2_ProcessEvent(&event)` 将 SDL 事件传递给 ImGui。

### 4. CMake 构建 (`CMakeLists.txt`)

- 移除 GLFW 相关的 `find_package` 和 `target_link_libraries`
- 使用 FetchContent 拉取 SDL2 源码编译：
  ```cmake
  FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.12
  )
  FetchContent_MakeAvailable(SDL2)
  ```
- 链接 `SDL2::SDL2` 和 `SDL2::SDL2main`
- ImGui backends 目录中替换 `imgui_impl_glfw.cpp` 为 `imgui_impl_sdl2.cpp`

### 5. Application 事件循环 (`engine/core/Application.cpp`)

当前：
```cpp
while (!m_window.shouldClose()) {
    m_window.pollEvents();
    onUpdate();
}
```

改为 Window 内部处理 SDL 事件循环，对外接口不变。Window::pollEvents() 内部改为：
```cpp
SDL_Event event;
while (SDL_PollEvent(&event)) {
    // 传给 ImGui（如果有回调）
    if (m_eventCallback) m_eventCallback(&event);
    // 处理窗口事件
    if (event.type == SDL_QUIT) m_shouldClose = true;
    if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED)
        // 触发 resize 回调
}
```

Editor 层在 ImGuiLayer 注册事件回调，调用 `ImGui_ImplSDL2_ProcessEvent`。

## 不变的部分

- Vulkan 渲染核心（Renderer、Pipeline、Buffer、SwapChain、RenderPass 等）
- 场景系统（Scene、Node、Camera）
- 资产系统（AssetManager、ShaderAsset、MaterialAsset）
- 所有 ImGui 面板逻辑（Hierarchy、Inspector、Project、SceneView、Console）
- ImGuizmo
- 着色器（SPIR-V）
- 纹理/模型/材质加载

## 风险

- SDL2 FetchContent 编译时间较长（首次）
- SDL2 的 Vulkan 扩展枚举方式与 GLFW 不同，需要调整 instance extension 获取逻辑
- ImGui SDL2 后端的输入映射可能有细微差异（需测试 ImGuizmo 交互）
