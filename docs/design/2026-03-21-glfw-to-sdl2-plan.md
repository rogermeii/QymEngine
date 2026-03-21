# GLFW → SDL2 替换实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将窗口/输入层从 GLFW 替换为 SDL2，在 Windows 上保持现有编辑器功能完全不变。

**Architecture:** SDL2 通过 FetchContent 引入源码编译。Window 类内部实现从 GLFW 切换到 SDL2，对外接口不变。VulkanContext 和 SwapChain 的 `GLFWwindow*` 参数改为 `SDL_Window*`。ImGui 后端从 `imgui_impl_glfw` 切换到 `imgui_impl_sdl2`。Window 类新增事件回调机制，供 ImGui 处理 SDL 事件。

**Tech Stack:** SDL2 2.30.x, Vulkan 1.3, ImGui (docking branch), C++17, CMake 3.20+

---

### Task 1: CMake 构建系统替换

**Files:**
- Modify: `CMakeLists.txt` (根目录)
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: 修改根 CMakeLists.txt — 移除 GLFW，引入 SDL2**

移除 GLFW imported library 定义（第16-21行），替换为 FetchContent：
```cmake
# 移除:
# add_library(glfw STATIC IMPORTED)
# set_target_properties(glfw PROPERTIES ...)

# 替换为:
include(FetchContent)
FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.12
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)
```

- [ ] **Step 2: 修改根 CMakeLists.txt — ImGui backend 替换**

将 imgui 库的源文件和链接从 GLFW 改为 SDL2：
```cmake
# ImGui sources: 替换 imgui_impl_glfw.cpp → imgui_impl_sdl2.cpp
file(GLOB IMGUI_SOURCES
    "3rd-party/imgui/*.cpp"
    "3rd-party/imgui/backends/imgui_impl_sdl2.cpp"
    "3rd-party/imgui/backends/imgui_impl_vulkan.cpp"
)
# imgui 链接: 替换 glfw → SDL2::SDL2-static
target_link_libraries(imgui PUBLIC Vulkan::Vulkan SDL2::SDL2-static)
```

同时让 imgui 的 include 路径能找到 SDL2 头文件（SDL2 通过 target 自动传播）。

- [ ] **Step 3: 修改 engine/CMakeLists.txt — 链接和定义替换**

```cmake
# 替换 glfw → SDL2::SDL2-static
target_link_libraries(QymEngineLib PUBLIC Vulkan::Vulkan SDL2::SDL2-static glm stb nlohmann_json tinyobjloader)
# 移除 GLFW_INCLUDE_VULKAN，其余不变
target_compile_definitions(QymEngineLib PUBLIC
    VK_USE_PLATFORM_WIN32_KHR
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)
```

- [ ] **Step 4: 验证 cmake 生成成功**

运行: `cmake -S . -B build3 -G "Visual Studio 17 2022" -A x64`
预期: 配置成功，SDL2 自动下载并配置完成（首次可能耗时几分钟）

---

### Task 2: Window 类替换

**Files:**
- Modify: `engine/core/Window.h`
- Modify: `engine/core/Window.cpp`

- [ ] **Step 1: 修改 Window.h — SDL2 类型替换**

```cpp
#pragma once
#include <SDL.h>
#include <SDL_vulkan.h>
#include <string>
#include <functional>

namespace QymEngine {

struct WindowProps {
    std::string title = "QymEngine";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool maximized = false;
};

class Window {
public:
    using FramebufferResizeCallback = std::function<void(int, int)>;
    using SDLEventCallback = std::function<void(const SDL_Event&)>;

    Window(const WindowProps& props = WindowProps{});
    ~Window();

    void pollEvents();
    bool shouldClose() const;
    void requestClose();

    SDL_Window* getNativeWindow() const { return m_window; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

    void setFramebufferResizeCallback(FramebufferResizeCallback cb) { m_resizeCallback = cb; }
    void setEventCallback(SDLEventCallback cb) { m_eventCallback = cb; }

private:
    SDL_Window* m_window = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_shouldClose = false;
    FramebufferResizeCallback m_resizeCallback;
    SDLEventCallback m_eventCallback;
};

} // namespace QymEngine
```

- [ ] **Step 2: 修改 Window.cpp — SDL2 实现**

```cpp
#include "Window.h"
#include <stdexcept>

namespace QymEngine {

Window::Window(const WindowProps& props)
    : m_width(props.width), m_height(props.height)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());

    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (props.maximized)
        flags |= SDL_WINDOW_MAXIMIZED;

    m_window = SDL_CreateWindow(
        props.title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        m_width, m_height, flags);

    if (!m_window)
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

    // Update actual size (may differ from requested when maximized)
    if (props.maximized) {
        int w, h;
        SDL_Vulkan_GetDrawableSize(m_window, &w, &h);
        m_width = static_cast<uint32_t>(w);
        m_height = static_cast<uint32_t>(h);
    }
}

Window::~Window()
{
    SDL_DestroyWindow(m_window);
    SDL_Quit();
}

void Window::pollEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Forward to ImGui / external listeners
        if (m_eventCallback)
            m_eventCallback(event);

        switch (event.type) {
        case SDL_QUIT:
            m_shouldClose = true;
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                m_width = static_cast<uint32_t>(event.window.data1);
                m_height = static_cast<uint32_t>(event.window.data2);
                if (m_resizeCallback)
                    m_resizeCallback(event.window.data1, event.window.data2);
            }
            break;
        }
    }
}

bool Window::shouldClose() const
{
    return m_shouldClose;
}

void Window::requestClose()
{
    m_shouldClose = true;
}

} // namespace QymEngine
```

---

### Task 3: VulkanContext 替换

**Files:**
- Modify: `engine/renderer/VulkanContext.h`
- Modify: `engine/renderer/VulkanContext.cpp`

- [ ] **Step 1: 修改 VulkanContext.h — GLFWwindow* → SDL_Window***

将前向声明 `struct GLFWwindow;` 替换为 `struct SDL_Window;`，`init(GLFWwindow*)` → `init(SDL_Window*)`，成员变量 `GLFWwindow* m_window` → `SDL_Window* m_window`。

- [ ] **Step 2: 修改 VulkanContext.cpp — Surface 创建和扩展枚举**

替换 `#include <GLFW/glfw3.h>` 为 `#include <SDL.h>` + `#include <SDL_vulkan.h>`。

`getRequiredExtensions()` 改为：
```cpp
std::vector<const char*> VulkanContext::getRequiredExtensions()
{
    unsigned int count = 0;
    SDL_Vulkan_GetInstanceExtensions(m_window, &count, nullptr);
    std::vector<const char*> extensions(count);
    SDL_Vulkan_GetInstanceExtensions(m_window, &count, extensions.data());

    if (s_enableValidationLayers)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    return extensions;
}
```

注意：SDL_Vulkan_GetInstanceExtensions 需要 SDL_Window*，因此需要把调用时机调整到 createSurface 之后或将 m_window 在 createInstance 之前保存。当前代码中 init() 先调用 createInstance() 再 createSurface()，而 getRequiredExtensions() 在 createInstance() 中被调用。需要确保此时 m_window 已赋值。

`createSurface()` 改为：
```cpp
void VulkanContext::createSurface()
{
    if (SDL_Vulkan_CreateSurface(m_window, m_instance, &m_surface) != SDL_TRUE)
        throw std::runtime_error("failed to create Vulkan surface!");
}
```

---

### Task 4: SwapChain 和 Renderer 中的 GLFW 引用替换

**Files:**
- Modify: `engine/renderer/SwapChain.h`
- Modify: `engine/renderer/SwapChain.cpp`
- Modify: `engine/renderer/Renderer.cpp`

- [ ] **Step 1: 修改 SwapChain.h — GLFWwindow* → SDL_Window***

将 `struct GLFWwindow;` 替换为 `struct SDL_Window;`。函数签名：
- `void create(VulkanContext& ctx, GLFWwindow* window)` → `void create(VulkanContext& ctx, SDL_Window* window)`
- `VkExtent2D chooseSwapExtent(..., GLFWwindow* window)` → `VkExtent2D chooseSwapExtent(..., SDL_Window* window)`

- [ ] **Step 2: 修改 SwapChain.cpp — glfwGetFramebufferSize 替换**

替换 `#include <GLFW/glfw3.h>` 为 `#include <SDL.h>` + `#include <SDL_vulkan.h>`。

`chooseSwapExtent()` 中：
```cpp
// 替换: glfwGetFramebufferSize(window, &width, &height);
SDL_Vulkan_GetDrawableSize(window, &width, &height);
```

- [ ] **Step 3: 修改 Renderer.cpp — recreateSwapChain 中 GLFW 调用替换**

替换 `#include <GLFW/glfw3.h>`（如有）为 SDL 头文件。

`recreateSwapChain()` 中：
```cpp
// 替换 GLFWwindow* 为 SDL_Window*
SDL_Window* nativeWindow = m_window->getNativeWindow();
int width = 0, height = 0;
SDL_Vulkan_GetDrawableSize(nativeWindow, &width, &height);
while (width == 0 || height == 0) {
    SDL_Vulkan_GetDrawableSize(nativeWindow, &width, &height);
    SDL_WaitEvent(nullptr);  // 替换 glfwWaitEvents()
}
```

---

### Task 5: ImGuiLayer SDL2 后端替换

**Files:**
- Modify: `editor/ImGuiLayer.cpp`

- [ ] **Step 1: 替换头文件和初始化**

```cpp
// 替换:
// #include <imgui_impl_glfw.h>
#include <imgui_impl_sdl2.h>

// init() 中替换:
// ImGui_ImplGlfw_InitForVulkan(renderer.getWindow()->getNativeWindow(), true);
ImGui_ImplSDL2_InitForVulkan(renderer.getWindow()->getNativeWindow());
```

- [ ] **Step 2: 替换帧更新和关闭**

```cpp
// beginFrame() 中替换:
// ImGui_ImplGlfw_NewFrame();
ImGui_ImplSDL2_NewFrame();

// shutdown() 中替换:
// ImGui_ImplGlfw_Shutdown();
ImGui_ImplSDL2_Shutdown();
```

- [ ] **Step 3: 注册 SDL 事件回调**

在 `init()` 末尾，向 Window 注册事件回调，将 SDL 事件传给 ImGui：
```cpp
renderer.getWindow()->setEventCallback([](const SDL_Event& event) {
    ImGui_ImplSDL2_ProcessEvent(&event);
});
```

---

### Task 6: EditorApp GLFW 引用清理

**Files:**
- Modify: `editor/EditorApp.cpp`

- [ ] **Step 1: 移除 GLFW include，替换窗口关闭调用**

```cpp
// 移除: #include <GLFW/glfw3.h>

// 替换 glfwSetWindowShouldClose:
// glfwSetWindowShouldClose(m_window->getNativeWindow(), GLFW_TRUE);
m_window->requestClose();
```

---

### Task 7: 编译验证

- [ ] **Step 1: 重新生成 CMake**

```bash
cmake -S . -B build3 -G "Visual Studio 17 2022" -A x64
```

- [ ] **Step 2: 编译**

```bash
cmake --build build3 --config Debug
```
修复所有编译错误。

- [ ] **Step 3: 启动编辑器验证**

```bash
build3/editor/Debug/QymEditor.exe
```
验证：
- 窗口正常创建和显示
- Scene View 渲染正常
- ImGui 面板交互正常（点击、拖拽、输入文字）
- Gizmo 正常工作（W/E/R 切换，拖拽操作）
- 右键+WASD 相机移动正常
- 窗口缩放正常
- 鼠标滚轮缩放正常
- Hierarchy 添加/删除节点正常

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "refactor: replace GLFW with SDL2 for cross-platform window management"
```
