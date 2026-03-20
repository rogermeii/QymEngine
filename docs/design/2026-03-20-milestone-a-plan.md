# QymEngine 里程碑 A 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将现有 Vulkan 单文件渲染 Demo 重构为模块化引擎 + ImGui 编辑器

**Architecture:** 渐进重构 — 从 main.cpp 逐步提取模块到 engine/（静态库）和 editor/（可执行文件），每步保持可编译可运行。双 RenderPass 架构：scene pass → offscreen，ImGui pass → swapchain。

**Tech Stack:** C++17, Vulkan 1.3, CMake 3.20+, GLFW 3.4, GLM, STB, Dear ImGui (docking)

**Spec:** `docs/design/2026-03-20-engine-editor-milestone-a-design.md`

---

## Task 0: CMake 迁移

**目标：** 用 CMake 替换 MSBuild，现有 main.cpp 原样编译运行

**Files:**
- Create: `CMakeLists.txt`
- Create: `engine/CMakeLists.txt`
- Modify: `QymEngine/main.cpp` (资源路径改用 ASSETS_DIR 宏)

- [ ] **Step 0.1: 创建根 CMakeLists.txt**

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(QymEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 资源路径宏
add_compile_definitions(ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

# Vulkan
find_package(Vulkan REQUIRED)

# GLFW（预编译库）
add_library(glfw STATIC IMPORTED)
set_target_properties(glfw PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/3rd-party/glfw-3.4.bin.WIN64/lib-vc2022/glfw3.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/3rd-party/glfw-3.4.bin.WIN64/include"
)

# GLM（header-only）
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${CMAKE_SOURCE_DIR}/3rd-party/glm-1.0.1-light")

# STB（header-only）
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${CMAKE_SOURCE_DIR}/3rd-party/stb-master")

# 引擎（暂时直接编译 main.cpp 为可执行文件）
add_subdirectory(engine)
```

- [ ] **Step 0.2: 创建 engine/CMakeLists.txt（临时，仅编译 main.cpp）**

```cmake
# engine/CMakeLists.txt
add_executable(QymEngine "${CMAKE_SOURCE_DIR}/QymEngine/main.cpp")

target_link_libraries(QymEngine PRIVATE
    Vulkan::Vulkan
    glfw
    glm
    stb
)

target_compile_definitions(QymEngine PRIVATE
    VK_USE_PLATFORM_WIN32_KHR
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)
```

- [ ] **Step 0.3: 更新 main.cpp 资源路径**

将 `readFile("shaders/vert.spv")` 改为 `readFile(std::string(ASSETS_DIR) + "/shaders/vert.spv")`，同理 frag.spv 和 texture.jpg。

- [ ] **Step 0.4: 迁移资源文件**

将 `QymEngine/shaders/` 和 `QymEngine/textures/` 移动到 `assets/shaders/` 和 `assets/textures/`。

- [ ] **Step 0.5: CMake 构建验证**

```bash
cd E:/MYQ/QymEngine
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
./build/engine/Debug/QymEngine.exe
```

Expected: 三角形渲染窗口正常显示。

- [ ] **Step 0.6: 下载 ImGui docking 分支**

将 ImGui docking 分支的核心文件放到 `3rd-party/imgui/`：
- `imgui.h`, `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`
- `imgui_internal.h`, `imconfig.h`, `imstb_rectpack.h`, `imstb_textedit.h`, `imstb_truetype.h`
- `imgui_demo.cpp`
- `backends/imgui_impl_glfw.h`, `backends/imgui_impl_glfw.cpp`
- `backends/imgui_impl_vulkan.h`, `backends/imgui_impl_vulkan.cpp`

在根 CMakeLists.txt 中添加 ImGui target（暂不链接）：

```cmake
# ImGui（docking 分支）
file(GLOB IMGUI_SOURCES
    "3rd-party/imgui/*.cpp"
    "3rd-party/imgui/backends/imgui_impl_glfw.cpp"
    "3rd-party/imgui/backends/imgui_impl_vulkan.cpp"
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    "${CMAKE_SOURCE_DIR}/3rd-party/imgui"
    "${CMAKE_SOURCE_DIR}/3rd-party/imgui/backends"
)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan glfw)
```

- [ ] **Step 0.7: 提交**

```bash
git add CMakeLists.txt engine/CMakeLists.txt assets/ 3rd-party/imgui/
git commit -m "build: migrate to CMake, add ImGui docking dependency"
```

---

## Task 1: 提取 Core 层

**目标：** 从 main.cpp 提取 Window、Application、Log 模块

**Files:**
- Create: `engine/core/Window.h`
- Create: `engine/core/Window.cpp`
- Create: `engine/core/Application.h`
- Create: `engine/core/Application.cpp`
- Create: `engine/core/Log.h`
- Create: `engine/core/Log.cpp`
- Modify: `QymEngine/main.cpp` (使用提取后的 Window/Application)
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1.1: 创建 Window 类**

```cpp
// engine/core/Window.h
#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <string>
#include <functional>

namespace QymEngine {

struct WindowProps {
    std::string title = "QymEngine";
    uint32_t width = 1280;
    uint32_t height = 720;
};

class Window {
public:
    using FramebufferResizeCallback = std::function<void(int, int)>;

    Window(const WindowProps& props = WindowProps{});
    ~Window();

    void pollEvents();
    bool shouldClose() const;

    GLFWwindow* getNativeWindow() const { return m_window; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

    void setFramebufferResizeCallback(FramebufferResizeCallback cb) { m_resizeCallback = cb; }

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* m_window = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    FramebufferResizeCallback m_resizeCallback;
};

} // namespace QymEngine
```

```cpp
// engine/core/Window.cpp
#include "Window.h"
#include <stdexcept>

namespace QymEngine {

Window::Window(const WindowProps& props)
    : m_width(props.width), m_height(props.height)
{
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    m_window = glfwCreateWindow(m_width, m_height, props.title.c_str(), nullptr, nullptr);
    if (!m_window)
        throw std::runtime_error("Failed to create GLFW window!");

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);
}

Window::~Window()
{
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

void Window::pollEvents()
{
    glfwPollEvents();
}

bool Window::shouldClose() const
{
    return glfwWindowShouldClose(m_window);
}

void Window::framebufferResizeCallback(GLFWwindow* window, int width, int height)
{
    auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    self->m_width = width;
    self->m_height = height;
    if (self->m_resizeCallback)
        self->m_resizeCallback(width, height);
}

} // namespace QymEngine
```

- [ ] **Step 1.2: 创建 Application 基类**

```cpp
// engine/core/Application.h
#pragma once
#include "Window.h"
#include <memory>

namespace QymEngine {

class Application {
public:
    Application(const WindowProps& props = WindowProps{});
    virtual ~Application() = default;

    void run();

    Window& getWindow() { return *m_window; }

protected:
    virtual void onInit() {}
    virtual void onUpdate() {}
    virtual void onShutdown() {}

    std::unique_ptr<Window> m_window;
};

} // namespace QymEngine
```

```cpp
// engine/core/Application.cpp
#include "Application.h"

namespace QymEngine {

Application::Application(const WindowProps& props)
    : m_window(std::make_unique<Window>(props))
{
}

void Application::run()
{
    onInit();

    while (!m_window->shouldClose())
    {
        m_window->pollEvents();
        onUpdate();
    }

    onShutdown();
}

} // namespace QymEngine
```

- [ ] **Step 1.3: 创建 Log 模块**

```cpp
// engine/core/Log.h
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <mutex>

namespace QymEngine {

enum class LogLevel { Info, Warn, Error };

struct LogEntry {
    LogLevel level;
    std::string message;
};

class Log {
public:
    using Callback = std::function<void(const LogEntry&)>;

    static void info(const std::string& msg);
    static void warn(const std::string& msg);
    static void error(const std::string& msg);

    static void addCallback(Callback cb);
    static const std::vector<LogEntry>& getEntries();

private:
    static void log(LogLevel level, const std::string& msg);

    static std::vector<LogEntry> s_entries;
    static std::vector<Callback> s_callbacks;
    static std::mutex s_mutex;
};

} // namespace QymEngine
```

```cpp
// engine/core/Log.cpp
#include "Log.h"
#include <iostream>

namespace QymEngine {

std::vector<LogEntry> Log::s_entries;
std::vector<Log::Callback> Log::s_callbacks;
std::mutex Log::s_mutex;

void Log::info(const std::string& msg)  { log(LogLevel::Info, msg); }
void Log::warn(const std::string& msg)  { log(LogLevel::Warn, msg); }
void Log::error(const std::string& msg) { log(LogLevel::Error, msg); }

void Log::addCallback(Callback cb)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_callbacks.push_back(std::move(cb));
}

const std::vector<LogEntry>& Log::getEntries()
{
    return s_entries;
}

void Log::log(LogLevel level, const std::string& msg)
{
    std::lock_guard<std::mutex> lock(s_mutex);

    LogEntry entry{level, msg};
    s_entries.push_back(entry);

    const char* prefix = "";
    switch (level) {
        case LogLevel::Info:  prefix = "[INFO]";  break;
        case LogLevel::Warn:  prefix = "[WARN]";  break;
        case LogLevel::Error: prefix = "[ERROR]"; break;
    }
    std::cout << prefix << " " << msg << std::endl;

    for (auto& cb : s_callbacks)
        cb(entry);
}

} // namespace QymEngine
```

- [ ] **Step 1.4: 更新 engine/CMakeLists.txt**

将 engine 改为静态库，包含 core 源文件，main.cpp 暂作为独立可执行文件：

```cmake
# engine/CMakeLists.txt

# 引擎静态库
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
)
add_library(QymEngineLib STATIC ${ENGINE_SOURCES})
target_include_directories(QymEngineLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(QymEngineLib PUBLIC Vulkan::Vulkan glfw glm stb)
target_compile_definitions(QymEngineLib PUBLIC
    VK_USE_PLATFORM_WIN32_KHR
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

# 临时：现有 main.cpp 直接编译为可执行文件
add_executable(QymEngine "${CMAKE_SOURCE_DIR}/QymEngine/main.cpp")
target_link_libraries(QymEngine PRIVATE QymEngineLib)
```

- [ ] **Step 1.5: 构建验证**

```bash
cmake --build build --config Debug
./build/engine/Debug/QymEngine.exe
```

Expected: 编译通过，三角形渲染正常。Core 模块已编译到静态库但 main.cpp 暂未使用。

- [ ] **Step 1.6: 提交**

```bash
git add engine/core/ engine/CMakeLists.txt
git commit -m "feat: extract Window, Application, Log from core layer"
```

---

## Task 2: 提取 Renderer 层

**目标：** 从 main.cpp 提取所有 Vulkan 渲染代码到 engine/renderer/

**Files:**
- Create: `engine/renderer/VulkanContext.h/cpp`
- Create: `engine/renderer/SwapChain.h/cpp`
- Create: `engine/renderer/RenderPass.h/cpp`
- Create: `engine/renderer/Pipeline.h/cpp`
- Create: `engine/renderer/Buffer.h/cpp`
- Create: `engine/renderer/Texture.h/cpp`
- Create: `engine/renderer/Descriptor.h/cpp`
- Create: `engine/renderer/CommandManager.h/cpp`
- Create: `engine/renderer/Renderer.h/cpp`
- Modify: `QymEngine/main.cpp` → 改为使用 Renderer 接口
- Modify: `engine/CMakeLists.txt`

**提取策略：** 这是最大的一步。按以下顺序提取，每个子步骤后编译验证：

- [ ] **Step 2.1: 提取 VulkanContext**

从 main.cpp 提取以下函数和成员到 `VulkanContext`：
- `createInstance()`, `setupDebugMessenger()`, `createSurface()`, `pickPhysicalDevice()`, `createLogicalDevice()`
- 辅助函数：`checkValidationLayerSupport()`, `getRequiredExtensions()`, `debugCallback()`, `populateDebugMessengerCreateInfo()`, `printInstanceExtensions()`, `rateDeviceSuitability()`, `checkDeviceExtensionSupport()`, `findQueueFamilies()`
- 全局函数：`CreateDebugUtilsMessengerEXT()`, `DestroyDebugUtilsMessengerEXT()`
- 结构体：`QueueFamilyIndices`
- 成员变量：`instance`, `debugMessenger`, `physicalDevice`, `device`, `graphicsQueue`, `presentQueue`, `surface`
- 常量：`validationLayers`, `deviceExtensions`, `enableValidationLayers`

```cpp
// engine/renderer/VulkanContext.h
#pragma once
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <vector>
#include <optional>

namespace QymEngine {

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    bool isComplete() { return graphicsFamily.has_value() && presentFamily.has_value(); }
};

class VulkanContext {
public:
    void init(GLFWwindow* window);
    void shutdown();

    VkInstance getInstance() const { return m_instance; }
    VkDevice getDevice() const { return m_device; }
    VkPhysicalDevice getPhysicalDevice() const { return m_physicalDevice; }
    VkQueue getGraphicsQueue() const { return m_graphicsQueue; }
    VkQueue getPresentQueue() const { return m_presentQueue; }
    VkSurfaceKHR getSurface() const { return m_surface; }
    VkCommandPool getCommandPool() const { return m_commandPool; }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createCommandPool();

    bool checkValidationLayerSupport();
    std::vector<const char*> getRequiredExtensions();
    int rateDeviceSuitability(VkPhysicalDevice device);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device);

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT*, void*);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
};

} // namespace QymEngine
```

实现：将 main.cpp 中对应函数逻辑原样搬迁到 `VulkanContext.cpp`，成员变量加 `m_` 前缀。

- [ ] **Step 2.2: 提取 SwapChain**

从 main.cpp 提取：
- `createSwapChain()`, `createImageViews()`, `createFramebuffers()`, `recreateSwapChain()`, `cleanupSwapChain()`
- 辅助函数：`querySwapChainSupport()`, `chooseSwapSurfaceFormat()`, `chooseSwapPresentMode()`, `chooseSwapExtent()`
- 结构体：`SwapChainSupportDetails`
- 同步对象：`createSyncObjects()`, fences, semaphores
- 成员：`swapChain`, `swapChainImages`, `swapChainImageFormat`, `swapChainExtent`, `swapChainImageViews`, `swapChainFramebuffers`, semaphores, fences

SwapChain 接收 `VulkanContext&` 引用，包含 `acquireNextImage()` 和 `present()` 方法。

- [ ] **Step 2.3: 提取 RenderPass**

从 main.cpp 提取 `createRenderPass()` 和相关成员。通用化为接受附件配置参数。

- [ ] **Step 2.4: 提取 Pipeline**

从 main.cpp 提取：
- `createGraphicsPipeline()`, `createShaderModule()`, `readFile()`
- `createDescriptorSetLayout()`
- 成员：`graphicsPipeline`, `pipelineLayout`, `descriptorSetLayout`

- [ ] **Step 2.5: 提取 Buffer**

从 main.cpp 提取：
- `createBuffer()`, `copyBuffer()`, `createVertexBuffer()`, `createIndexBuffer()`, `createUniformBuffers()`
- 结构体：`Vertex`, `UniformBufferObject`
- 顶点/索引数据
- 成员：vertexBuffer, indexBuffer, uniformBuffers 等

- [ ] **Step 2.6: 提取 Texture**

从 main.cpp 提取：
- `createImage()`, `createImageView()`, `transitionImageLayout()`, `copyBufferToImage()`
- `createTextureImage()`, `createTextureImageView()`, `createTextureSampler()`
- 成员：textureImage, textureImageView, textureSampler 等
- **`STB_IMAGE_IMPLEMENTATION` 仅在 `Texture.cpp` 中定义**

- [ ] **Step 2.7: 提取 Descriptor**

从 main.cpp 提取：
- `createDescriptorPool()`, `createDescriptorSets()`
- 成员：descriptorPool, descriptorSets

- [ ] **Step 2.8: 提取 CommandManager**

从 main.cpp 提取：
- `createCommandBuffer()`, `beginSingleTimeCommands()`, `endSingleTimeCommands()`
- `recordCommandBuffer()`
- 成员：commandBuffers

- [ ] **Step 2.9: 创建 Renderer 高层接口**

```cpp
// engine/renderer/Renderer.h
#pragma once
#include "VulkanContext.h"
#include "SwapChain.h"
#include "RenderPass.h"
#include "Pipeline.h"
#include "Buffer.h"
#include "Texture.h"
#include "Descriptor.h"
#include "CommandManager.h"

namespace QymEngine {

class Renderer {
public:
    void init(Window& window);
    void shutdown();

    // 每帧调用
    bool beginFrame();  // acquire image, 返回 false 表示需要 recreate
    void endFrame();    // submit + present

    // 场景渲染（当前：硬编码三角形）
    void drawScene();

    VulkanContext& getContext() { return m_context; }
    SwapChain& getSwapChain() { return m_swapChain; }
    VkCommandBuffer getCurrentCommandBuffer();
    uint32_t getCurrentFrame() const { return m_currentFrame; }

    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

private:
    VulkanContext m_context;
    SwapChain m_swapChain;
    // ... 其他子模块

    uint32_t m_currentFrame = 0;
    uint32_t m_imageIndex = 0;
};

} // namespace QymEngine
```

协调 `drawFrame()` 的逻辑：beginFrame (wait fence + acquire) → drawScene (record commands) → endFrame (submit + present)。

- [ ] **Step 2.10: 更新 main.cpp 使用 Renderer**

```cpp
// QymEngine/main.cpp（精简版）
#include "core/Application.h"
#include "renderer/Renderer.h"

class TriangleApp : public QymEngine::Application {
public:
    TriangleApp() : Application({"QymEngine", 800, 600}) {}

protected:
    void onInit() override {
        m_renderer.init(getWindow());
    }

    void onUpdate() override {
        if (m_renderer.beginFrame()) {
            m_renderer.drawScene();
            m_renderer.endFrame();
        }
    }

    void onShutdown() override {
        m_renderer.shutdown();
    }

private:
    QymEngine::Renderer m_renderer;
};

int main() {
    TriangleApp app;
    try { app.run(); }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
```

- [ ] **Step 2.11: 更新 CMakeLists.txt**

engine/CMakeLists.txt 加入 renderer/ 源文件到静态库。

- [ ] **Step 2.12: 构建验证**

```bash
cmake --build build --config Debug
./build/engine/Debug/QymEngine.exe
```

Expected: 渲染结果与 Task 1 完全一致（旋转纹理四边形）。

- [ ] **Step 2.13: 提交**

```bash
git add engine/renderer/ engine/CMakeLists.txt QymEngine/main.cpp
git commit -m "refactor: extract Vulkan renderer modules from main.cpp"
```

---

## Task 3: ImGui 集成

**目标：** 在渲染循环中加入 ImGui，使用独立 RenderPass 渲染到 swapchain

**Files:**
- Create: `editor/CMakeLists.txt`
- Create: `editor/ImGuiLayer.h`
- Create: `editor/ImGuiLayer.cpp`
- Create: `editor/main.cpp`
- Modify: `CMakeLists.txt` (添加 editor subdirectory)

- [ ] **Step 3.1: 创建 editor/CMakeLists.txt**

```cmake
# editor/CMakeLists.txt
add_executable(QymEditor main.cpp ImGuiLayer.cpp)
target_link_libraries(QymEditor PRIVATE QymEngineLib imgui)
target_include_directories(QymEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
```

根 CMakeLists.txt 添加 `add_subdirectory(editor)`。

- [ ] **Step 3.2: 创建 ImGuiLayer**

```cpp
// editor/ImGuiLayer.h
#pragma once
#include "renderer/Renderer.h"

namespace QymEngine {

class ImGuiLayer {
public:
    void init(Renderer& renderer);
    void shutdown();

    void beginFrame();
    void endFrame(VkCommandBuffer cmd);

    void enableDocking();

private:
    void createImGuiRenderPass();
    void createImGuiFramebuffers();
    void recreateFramebuffers();

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    Renderer* m_renderer = nullptr;
};

} // namespace QymEngine
```

实现要点：
- `init()`: 创建 ImGui 专用描述符池，调用 `ImGui_ImplGlfw_InitForVulkan` 和 `ImGui_ImplVulkan_Init`
- ImGui 使用独立 VkRenderPass，finalLayout = `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`
- `beginFrame()`: `ImGui_ImplVulkan_NewFrame()` + `ImGui_ImplGlfw_NewFrame()` + `ImGui::NewFrame()`
- `endFrame()`: `ImGui::Render()` + 录制 ImGui RenderPass 命令 + `ImGui_ImplVulkan_RenderDrawData`

- [ ] **Step 3.3: 创建 editor/main.cpp**

```cpp
// editor/main.cpp
#include "core/Application.h"
#include "renderer/Renderer.h"
#include "ImGuiLayer.h"
#include <imgui.h>

class EditorApp : public QymEngine::Application {
public:
    EditorApp() : Application({"QymEngine Editor", 1280, 720}) {}

protected:
    void onInit() override {
        m_renderer.init(getWindow());
        m_imguiLayer.init(m_renderer);
    }

    void onUpdate() override {
        if (!m_renderer.beginFrame())
            return;

        // 场景渲染（直接到 swapchain，Step 5 改为 offscreen）
        m_renderer.drawScene();

        // ImGui 渲染
        m_imguiLayer.beginFrame();
        m_imguiLayer.enableDocking();
        ImGui::ShowDemoWindow(); // 临时：显示 Demo Window
        m_imguiLayer.endFrame(m_renderer.getCurrentCommandBuffer());

        m_renderer.endFrame();
    }

    void onShutdown() override {
        m_imguiLayer.shutdown();
        m_renderer.shutdown();
    }

private:
    QymEngine::Renderer m_renderer;
    QymEngine::ImGuiLayer m_imguiLayer;
};

int main() {
    EditorApp app;
    try { app.run(); }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
```

- [ ] **Step 3.4: 构建验证**

```bash
cmake --build build --config Debug
./build/editor/Debug/QymEditor.exe
```

Expected: 窗口中显示三角形渲染 + ImGui Demo Window，可以拖拽 ImGui 面板。

- [ ] **Step 3.5: 提交**

```bash
git add editor/ CMakeLists.txt
git commit -m "feat: integrate ImGui with Vulkan backend, docking enabled"
```

---

## Task 4: 编辑器面板

**目标：** 创建 5 个面板壳子，组装 Docking 布局

**Files:**
- Create: `editor/EditorApp.h`
- Create: `editor/EditorApp.cpp`
- Create: `editor/panels/SceneViewPanel.h/cpp`
- Create: `editor/panels/HierarchyPanel.h/cpp`
- Create: `editor/panels/InspectorPanel.h/cpp`
- Create: `editor/panels/ProjectPanel.h/cpp`
- Create: `editor/panels/ConsolePanel.h/cpp`
- Modify: `editor/main.cpp`
- Modify: `editor/CMakeLists.txt`

- [ ] **Step 4.1: 创建 SceneViewPanel（占位）**

```cpp
// editor/panels/SceneViewPanel.h
#pragma once
namespace QymEngine {
class SceneViewPanel {
public:
    void onImGuiRender();
};
}

// editor/panels/SceneViewPanel.cpp
#include "SceneViewPanel.h"
#include <imgui.h>
namespace QymEngine {
void SceneViewPanel::onImGuiRender() {
    ImGui::Begin("Scene View");
    ImGui::Text("TODO: Offscreen render target");
    ImGui::End();
}
}
```

- [ ] **Step 4.2: 创建 HierarchyPanel（占位）**

```cpp
void HierarchyPanel::onImGuiRender() {
    ImGui::Begin("Hierarchy");
    ImGui::Text("Scene");
    ImGui::Indent();
    ImGui::Selectable("Main Camera");
    ImGui::Selectable("Directional Light");
    ImGui::Selectable("Triangle");
    ImGui::Unindent();
    ImGui::End();
}
```

- [ ] **Step 4.3: 创建 InspectorPanel（占位）**

```cpp
void InspectorPanel::onImGuiRender() {
    ImGui::Begin("Inspector");
    ImGui::Text("Triangle");
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        float pos[3] = {0, 0, 0};
        float rot[3] = {0, 0, 0};
        float scale[3] = {1, 1, 1};
        ImGui::DragFloat3("Position", pos, 0.1f);
        ImGui::DragFloat3("Rotation", rot, 0.1f);
        ImGui::DragFloat3("Scale", scale, 0.1f);
    }
    ImGui::End();
}
```

- [ ] **Step 4.4: 创建 ProjectPanel（占位）**

```cpp
void ProjectPanel::onImGuiRender() {
    ImGui::Begin("Project");
    ImGui::Text("assets/");
    ImGui::Indent();
    ImGui::Selectable("shaders/");
    ImGui::Selectable("textures/");
    ImGui::Unindent();
    ImGui::End();
}
```

- [ ] **Step 4.5: 创建 ConsolePanel**

```cpp
// editor/panels/ConsolePanel.h
#pragma once
#include "core/Log.h"
#include <vector>

namespace QymEngine {
class ConsolePanel {
public:
    void init();
    void onImGuiRender();

private:
    std::vector<LogEntry> m_logs;
    bool m_autoScroll = true;
    int m_levelFilter = -1; // -1 = all
};
}

// editor/panels/ConsolePanel.cpp
void ConsolePanel::init() {
    Log::addCallback([this](const LogEntry& entry) {
        m_logs.push_back(entry);
    });
}

void ConsolePanel::onImGuiRender() {
    ImGui::Begin("Console");

    if (ImGui::Button("Clear"))
        m_logs.clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);

    ImGui::Separator();

    ImGui::BeginChild("LogRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto& entry : m_logs) {
        ImVec4 color;
        switch (entry.level) {
            case LogLevel::Info:  color = ImVec4(1, 1, 1, 1); break;
            case LogLevel::Warn:  color = ImVec4(1, 1, 0, 1); break;
            case LogLevel::Error: color = ImVec4(1, 0.4f, 0.4f, 1); break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(entry.message.c_str());
        ImGui::PopStyleColor();
    }
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}
```

- [ ] **Step 4.6: 创建 EditorApp，组装 Docking 布局**

将 editor/main.cpp 中的 EditorApp 逻辑移到 EditorApp.h/cpp：
- `onInit()` 初始化所有面板，调用 `Log::info("QymEngine Editor initialized")`
- `onUpdate()` 中设置 Docking 空间，按布局排列面板
- ImGui Docking 初始布局通过 `ImGui::DockBuilderAddNode` / `DockBuilderSplitNode` 编程设置

- [ ] **Step 4.7: 更新 editor/CMakeLists.txt**

添加 panels/*.cpp 到编译列表。

- [ ] **Step 4.8: 构建验证**

```bash
cmake --build build --config Debug
./build/editor/Debug/QymEditor.exe
```

Expected: 5 个面板以 Docking 布局显示（左 Hierarchy，中 Scene View 占位，右 Inspector，底部 Project/Console Tab），Console 显示初始化日志。

- [ ] **Step 4.9: 提交**

```bash
git add editor/
git commit -m "feat: add editor panels - SceneView, Hierarchy, Inspector, Project, Console"
```

---

## Task 5: Scene View 渲染到纹理

**目标：** 将场景渲染到 offscreen framebuffer，通过 ImGui::Image 显示在 SceneView 面板中

**Files:**
- Modify: `editor/panels/SceneViewPanel.h/cpp`
- Modify: `engine/renderer/Renderer.h/cpp` (添加 offscreen 支持)

- [ ] **Step 5.1: 创建 offscreen framebuffer 资源**

在 Renderer 或 SceneViewPanel 中添加：
- `VkImage` 颜色附件 + `VkDeviceMemory`
- `VkImageView` 颜色 image view
- `VkImage` 深度附件 + `VkDeviceMemory`（格式 `VK_FORMAT_D32_SFLOAT`）
- `VkImageView` 深度 image view
- `VkFramebuffer` offscreen framebuffer
- `VkRenderPass` offscreen render pass（颜色 finalLayout = `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`）
- `VkSampler` 用于 ImGui 采样
- `VkDescriptorSet` 通过 `ImGui_ImplVulkan_AddTexture` 创建

初始大小：800x600，后续跟随面板大小。

- [ ] **Step 5.2: 修改渲染流程为双 pass**

每帧流程变为：
1. `beginFrame()` — wait fence, acquire swapchain image
2. Begin command buffer
3. **Scene RenderPass** — bind offscreen framebuffer, draw scene, end render pass
4. **ImGui RenderPass** — bind swapchain framebuffer, render ImGui, end render pass
5. End command buffer
6. `endFrame()` — submit, present

- [ ] **Step 5.3: 更新 SceneViewPanel**

```cpp
void SceneViewPanel::onImGuiRender() {
    ImGui::Begin("Scene View");

    ImVec2 viewportSize = ImGui::GetContentRegionAvail();

    // 检测 resize
    if (viewportSize.x != m_width || viewportSize.y != m_height) {
        m_width = static_cast<uint32_t>(viewportSize.x);
        m_height = static_cast<uint32_t>(viewportSize.y);
        m_needsResize = true;
    }

    // 显示 offscreen 渲染结果
    if (m_descriptorSet != VK_NULL_HANDLE) {
        ImGui::Image((ImTextureID)m_descriptorSet, viewportSize);
    }

    ImGui::End();
}
```

- [ ] **Step 5.4: 实现 offscreen resize**

当 `m_needsResize == true` 时，在下一帧 beginFrame 之前：
1. `vkDeviceWaitIdle(device)` — 确保资源不在使用
2. 销毁旧的 offscreen 资源（image, imageView, framebuffer, descriptorSet）
3. 用新尺寸重新创建
4. 通过 `ImGui_ImplVulkan_AddTexture` 创建新的 descriptorSet
5. 清除 dirty flag

- [ ] **Step 5.5: 构建验证**

```bash
cmake --build build --config Debug
./build/editor/Debug/QymEditor.exe
```

Expected: 旋转纹理四边形在 Scene View 面板内渲染，拖拽调整面板大小时视口跟随，其他面板正常显示。

- [ ] **Step 5.6: 提交**

```bash
git add editor/ engine/
git commit -m "feat: render scene to offscreen texture, display in SceneView panel"
```

---

## 里程碑 A 完成检查

- [ ] CMake 构建通过（Debug + Release）
- [ ] 编辑器启动显示 5 面板 Docking 布局
- [ ] Scene View 内渲染旋转纹理四边形
- [ ] Console 显示初始化日志
- [ ] 面板可自由拖拽/调整大小
- [ ] 无 Vulkan validation layer 错误
