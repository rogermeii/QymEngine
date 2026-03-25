# Android 引擎渲染 MVP 设计

**日期**: 2026-03-21
**目标**: 在 Android 真机上运行一个硬编码的旋转立方体，验证 Vulkan 渲染管线在 Android 上能正常工作。

## 背景

引擎已完成 GLFW → SDL2 替换，SDL2 原生支持 Android。现在需要搭建 Android 构建管线，将引擎核心（不含编辑器）编译为 Android 应用。

## 环境

- Android SDK: `C:\Users\meiyuanqiao\AppData\Local\Android\Sdk`
- NDK: 29.0.14206865（Perforce SDK 路径下）
- Java: JDK 17
- 目标: 真机运行（Vulkan 支持的 Android 手机）

## 架构

### Android 入口层

使用 SDL2 的 Android 支持。SDL2 提供 `SDLActivity`（Java），负责创建 Android Activity 和 Surface，然后调用 native 的 `SDL_main` 函数。不需要自己写 Java/Kotlin 代码。

### 项目结构

```
android/
├── app/
│   ├── build.gradle              # Android 构建配置
│   ├── src/
│   │   └── main/
│   │       ├── AndroidManifest.xml
│   │       ├── java/             # SDL2 的 SDLActivity（由 SDL2 提供，符号链接或拷贝）
│   │       └── jniLibs/          # 编译产物输出（.so）
│   └── assets/
│       └── shaders/              # 预编译 SPIR-V 着色器
├── build.gradle                  # 根构建文件
├── settings.gradle
├── gradle.properties
└── CMakeLists.txt                # Android native 构建（engine + SDL2 + main）
```

### 入口代码 (`android/main.cpp`)

硬编码创建场景：
- 初始化 SDL2 + Vulkan
- 创建 Renderer
- 创建一个 Cube 节点
- 设置固定相机
- 主循环：每帧旋转 Cube，调用 Renderer 渲染
- 处理 SDL_QUIT 退出

### 引擎复用

直接复用现有模块：
- `engine/renderer/` — Renderer, Pipeline, Buffer, SwapChain, VulkanContext, etc.
- `engine/scene/` — Scene, Node, Camera, Transform
- `engine/core/` — Window (SDL2)

不使用：
- `editor/` — 全部不编译
- `engine/asset/` — MVP 不需要文件加载

### 平台适配

1. **Vulkan 平台宏**: CMake 中 Android 用 `VK_USE_PLATFORM_ANDROID_KHR`，Windows 保持 `VK_USE_PLATFORM_WIN32_KHR`
2. **设备选择**: VulkanContext 的设备评分逻辑在移动端不需要偏好独显，但当前实现已能选择可用设备，MVP 阶段无需修改
3. **着色器**: 预编译的 `.spv` 文件打包进 APK assets，通过 SDL2 的 `SDL_RWFromFile` 或 Android AAssetManager 读取
4. **ASSETS_DIR**: MVP 硬编码场景不依赖文件加载。着色器加载路径需要适配（从 APK assets 读取而非文件系统）

### 着色器加载适配

当前 `Pipeline::readFile()` 使用 `std::ifstream` 从文件系统读取 SPIR-V。Android 的 APK assets 不在文件系统中，需要改用 `SDL_RWFromFile` 或 `AAssetManager`。这是 MVP 中需要修改引擎代码的唯一部分。

方案：在 `Pipeline::readFile()` 中增加 Android 条件编译路径，使用 `SDL_RWFromFile("shaders/lit_vert.spv", "rb")` 读取。SDL2 在 Android 上会自动从 APK assets 读取。

## 不改动的部分

- Windows 版本的所有代码
- 编辑器 (ImGui)
- 资产加载系统（AssetManager）
- 触控输入

## 最终产物

一个可安装到 Android 手机的 APK，打开后全屏显示一个在灯光下旋转的 PBR 立方体。

## 风险

- SDL2 + Vulkan 在 Android 上的 Surface 创建可能需要调试
- 部分 Vulkan 扩展（如 `VK_KHR_PRESENT_ID`）在移动 GPU 上可能不支持，需要做可选扩展处理
- 着色器可能需要调整（移动 GPU 对某些 GLSL 特性支持不同）
- Gradle + CMake 交叉编译配置可能需要反复调试
