# QymEngine 编辑器 — 里程碑 A 设计文档

**日期**: 2026-03-20
**目标**: 从现有 Vulkan 渲染 Demo 扩展为带编辑器界面的引擎原型
**定位**: 学习项目，架构预留扩展余地

---

## 1. 概述

将现有的单文件 Vulkan 三角形渲染程序（`main.cpp`，约 1710 行）重构为模块化引擎 + 编辑器架构。里程碑 A 的交付目标是：

- 引擎代码按模块拆分，编译为静态库
- 编辑器基于 ImGui Docking，包含 5 个面板（Scene View / Hierarchy / Inspector / Project / Console）
- Scene View 中渲染现有三角形（offscreen → ImGui::Image）
- 各面板第一阶段为空壳/基础展示，不做交互编辑

## 2. 技术栈

| 项目 | 选型 |
|------|------|
| 语言 | C++17 |
| 图形 API | Vulkan 1.4 |
| 构建系统 | CMake（从 MSBuild 迁移） |
| 窗口管理 | GLFW 3.4 |
| 数学库 | GLM 1.0.1 |
| 编辑器 UI | Dear ImGui（docking 分支） |
| 图像加载 | STB |
| 平台 | Windows x64 |

## 3. 架构设计

### 3.1 目录结构

```
QymEngine/
├── CMakeLists.txt                 # 根 CMake
├── engine/
│   ├── CMakeLists.txt             # 静态库 QymEngine
│   ├── core/
│   │   ├── Application.h/cpp      # 主循环 Init → Update → Shutdown
│   │   ├── Window.h/cpp           # GLFW 窗口封装
│   │   ├── Input.h/cpp            # 键盘/鼠标状态查询
│   │   └── Log.h/cpp              # 日志系统（Info/Warn/Error + 回调）
│   └── renderer/
│       ├── VulkanContext.h/cpp     # Instance, PhysicalDevice, LogicalDevice, Queues
│       ├── SwapChain.h/cpp         # 交换链创建/重建/Acquire/Present
│       ├── Pipeline.h/cpp          # 着色器加载、管线状态配置
│       ├── RenderPass.h/cpp        # RenderPass 封装
│       ├── Buffer.h/cpp            # VBO, IBO, UBO 管理
│       ├── Texture.h/cpp           # 图像加载 → VkImage + 采样器
│       ├── Descriptor.h/cpp        # 描述符集/池/布局
│       ├── CommandManager.h/cpp    # 命令缓冲区管理
│       └── Renderer.h/cpp          # 高层渲染接口（beginFrame/endFrame/draw）
├── editor/
│   ├── CMakeLists.txt             # 可执行文件 QymEditor
│   ├── EditorApp.h/cpp            # 编辑器主程序，继承 Application
│   ├── ImGuiLayer.h/cpp           # ImGui 初始化、Vulkan+GLFW backend
│   └── panels/
│       ├── SceneViewPanel.h/cpp    # 场景渲染视口（offscreen → ImGui::Image）
│       ├── HierarchyPanel.h/cpp    # 场景节点树
│       ├── InspectorPanel.h/cpp    # 属性检查器
│       ├── ProjectPanel.h/cpp      # 资源目录浏览
│       └── ConsolePanel.h/cpp      # 日志输出面板
├── 3rd-party/
│   ├── glfw/                      # GLFW 3.4 预编译
│   ├── glm/                       # GLM header-only
│   ├── stb/                       # STB header-only
│   └── imgui/                     # Dear ImGui docking 分支
├── assets/
│   ├── shaders/                   # Triangle.vert/frag + compile.bat
│   └── textures/                  # texture.jpg
└── docs/
```

### 3.2 模块依赖关系

```
Editor 层（可执行文件 QymEditor）
    │
    │ 依赖
    ▼
Engine 层（静态库 QymEngine）
    │
    │ 依赖
    ▼
第三方库（Vulkan SDK / GLFW / GLM / ImGui / STB）
```

**关键原则：**
- **单向依赖**：Editor → Engine → 3rd-party，引擎层不知道编辑器的存在
- **Application 模式**：Engine 提供 `Application` 基类（Init/Update/Shutdown），`EditorApp` 继承并集成 ImGui
- **渲染到纹理**：Scene View 通过 offscreen framebuffer 渲染场景，再通过 `VkDescriptorSet` → `ImGui::Image` 显示

### 3.3 CMake 结构

```cmake
# 根 CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(QymEngine)

# 第三方依赖
add_subdirectory(3rd-party/imgui)  # 封装为 CMake target
find_package(Vulkan REQUIRED)
# GLFW: 预编译库，通过 target 引入
# GLM / STB: header-only，interface target

# 引擎静态库
add_subdirectory(engine)

# 编辑器可执行文件
add_subdirectory(editor)
```

## 4. 编辑器布局

采用经典四面板 + Tab 布局（类似 Unity），基于 ImGui Docking：

```
┌─────────────┬──────────────────────────┬───────────────┐
│             │                          │               │
│  Hierarchy  │       Scene View         │   Inspector   │
│  (场景树)    │   (Vulkan offscreen      │   (属性面板)   │
│             │    渲染结果)              │               │
│             │                          │               │
├─────────────┴──────────────────────────┴───────────────┤
│  [ Project ]  [ Console ]                              │
│  资源目录浏览 / 日志输出                                  │
└────────────────────────────────────────────────────────┘
```

- 所有面板可拖拽、停靠、调整大小
- 底部 Project 和 Console 为 Tab 切换
- Scene View 视口大小跟随面板大小

## 5. 模块职责

### 5.1 Engine — Core

| 模块 | 职责 |
|------|------|
| `Application` | 主循环框架（Init → Update → Shutdown），提供虚函数供子类覆写 |
| `Window` | 封装 GLFW 窗口创建/销毁/事件回调/大小查询 |
| `Input` | 静态查询接口：IsKeyPressed / IsMouseButtonDown / GetMousePosition |
| `Log` | 日志系统，支持 Info/Warn/Error 级别，回调注册机制（编辑器 Console 通过回调接收日志） |

### 5.2 Engine — Renderer

| 模块 | 职责 |
|------|------|
| `VulkanContext` | Vulkan Instance / Physical Device 选择 / Logical Device / Queue 获取 / Debug Messenger |
| `SwapChain` | 交换链创建与重建 / 图像获取与呈现 / 同步对象（Fence, Semaphore） |
| `RenderPass` | RenderPass 创建，附件描述，子通道配置 |
| `Pipeline` | 着色器模块加载（.spv）/ 图形管线状态配置 / 管线布局 |
| `Buffer` | 顶点缓冲区 / 索引缓冲区 / Uniform 缓冲区 / Staging 与设备本地内存传输 |
| `Texture` | STB 加载图像 → VkImage / Image View / Sampler 创建 / 布局转换 |
| `Descriptor` | 描述符集布局 / 描述符池 / 描述符集分配与更新 |
| `CommandManager` | 命令池 / 命令缓冲区分配与录制 / 单次命令便捷接口 |
| `Renderer` | 高层渲染接口：beginFrame / endFrame / drawMesh，协调上述模块 |

### 5.3 Editor

| 模块 | 职责 |
|------|------|
| `EditorApp` | 继承 Application，管理 ImGuiLayer 和所有面板的生命周期 |
| `ImGuiLayer` | ImGui 上下文初始化 / Vulkan+GLFW backend 配置 / NewFrame/Render 调用 / Docking 空间设置 |
| `SceneViewPanel` | 管理 offscreen framebuffer / 将渲染结果作为 ImGui::Image 显示 / 视口大小联动 |
| `HierarchyPanel` | 显示场景节点列表（第一阶段：静态硬编码列表） |
| `InspectorPanel` | 显示选中对象属性（第一阶段：静态 Transform 数据展示） |
| `ProjectPanel` | 显示 assets 目录内容（第一阶段：只读文件列表） |
| `ConsolePanel` | 显示日志输出，通过 Log 回调接收，支持按级别过滤和清除 |

## 6. 迭代计划

### Step 0 — CMake 迁移
- 创建根 `CMakeLists.txt`，引入 GLFW / GLM / STB / Vulkan
- 把现有 `main.cpp` 原样编译通过，确认运行正常
- 引入 ImGui（docking 分支）作为第三方依赖
- **验证**：CMake 构建 → 运行 → 看到三角形渲染窗口

### Step 1 — 提取 Core 层
- 从 `main.cpp` 提取 `Window` 类（GLFW 窗口创建/销毁/回调）
- 提取 `Application` 基类（Init → MainLoop → Shutdown 虚函数框架）
- 添加 `Log` 模块（Info/Warn/Error + 回调注册）
- 保持现有渲染逻辑在 `main.cpp` 中不动
- **验证**：编译通过，运行效果不变

### Step 2 — 提取 Renderer 层
- 提取 `VulkanContext`（Instance / Device / Queue）
- 提取 `SwapChain`（创建 / 重建 / Acquire / Present）
- 提取 `Pipeline`、`RenderPass`、`Buffer`、`Texture`、`Descriptor`、`CommandManager`
- 提取 `Renderer` 高层接口（beginFrame / endFrame）
- **验证**：渲染结果与 Step 1 完全一致

### Step 3 — ImGui 集成
- 创建 `ImGuiLayer`：初始化 ImGui Vulkan + GLFW backend
- 在主渲染循环中插入 ImGui 渲染 pass
- 启用 ImGui Docking，设置默认布局
- **验证**：窗口中出现可拖拽的 ImGui Demo Window

### Step 4 — 编辑器面板
- 创建 `EditorApp` 继承 `Application`
- 实现 5 个面板：SceneView / Hierarchy / Inspector / Project / Console
- 各面板用占位内容（标题 + "TODO" 文字）
- Console 面板接入 Log 系统，显示初始化日志
- **验证**：ImGui Docking 布局中显示 5 个面板，Console 有日志输出

### Step 5 — Scene View 渲染到纹理
- 创建 offscreen framebuffer + 颜色附件
- 将现有三角形渲染到 offscreen framebuffer
- 通过 `ImGui_ImplVulkan_AddTexture` 创建描述符集
- `ImGui::Image` 在 SceneView 面板中显示渲染结果
- **验证**：三角形在 Scene View 面板内渲染，调整面板大小时视口跟随

## 7. 参考项目

| 参考 | 用途 |
|------|------|
| Hazel Engine | 引擎整体架构、编辑器框架、Application 模式 |
| Piccolo (GAMES104) | Vulkan + ImGui 编辑器集成 |
| Diligent Engine | 渲染抽象层设计，后续多后端参考 |
| Godot Engine | 具体功能实现（Inspector 反射、Scene 序列化） |

## 8. 风险与注意事项

1. **offscreen 渲染同步**：Scene View 的 offscreen pass 需要和 ImGui 的 swapchain pass 正确同步（semaphore 依赖）
2. **ImGui Docking 分支**：需要使用 ImGui 的 docking 分支而非 master，API 略有不同
3. **渐进拆分纪律**：每一步必须保持可编译可运行，避免大规模重构导致长时间不可验证
4. **Vulkan 验证层**：开发阶段始终开启 validation layer，及时发现资源泄漏和同步错误
