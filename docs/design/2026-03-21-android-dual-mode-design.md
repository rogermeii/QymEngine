# Android 双模式设计：Editor + Runtime Player

**日期**: 2026-03-21
**目标**: Android 上支持两种运行模式 — 完整编辑器（默认）和 Runtime Player（带虚拟摇杆）。

## 模式

### Editor 模式（默认）

- 初始化 ImGui（imgui_impl_sdl2 + imgui_impl_vulkan）
- 渲染完整编辑器面板（Hierarchy、Inspector、Project、SceneView、Console）
- 触摸事件通过 SDL → ImGui 传递
- 和 Windows 版相同的渲染流程（ImGui 渲染到 swapchain）

### Runtime Player 模式（`-runtime-player`）

- 不渲染编辑器面板
- 全屏渲染场景（offscreen + blitToSwapchain）
- 左下角虚拟摇杆：控制相机前后左右移动
- 右下角虚拟摇杆：控制相机 orbit（旋转视角）
- 用 ImGui 绘制半透明摇杆 UI（最小化 ImGui 使用）

## 架构

### 入口

`android-cmake/main.cpp` 解析启动参数，分发到 EditorApp 或 RuntimeApp。

### 构建

Android CMake 需要编译：
- engine/ 全部源码
- editor/ 全部源码（EditorApp、ImGuiLayer、所有面板）
- imgui 库（含 imgui_impl_sdl2 + imgui_impl_vulkan）

### 文件

```
android-cmake/
├── CMakeLists.txt    — 编译 engine + editor + imgui
├── main.cpp          — 解析参数，分发模式
└── VirtualJoystick.h — 虚拟摇杆（ImGui 绘制 + 触摸输入）
```

### 虚拟摇杆

- 圆形底座（半透明灰色）+ 圆形手柄（半透明白色）
- 触摸拖拽手柄，手柄最大移动范围不超过底座半径
- 输出归一化方向向量 (-1 到 +1)
- 左摇杆：方向映射到相机 forward/right 移动
- 右摇杆：方向映射到相机 yaw/pitch orbit

### 字体适配

ImGuiLayer 中的 `C:/Windows/Fonts/msyh.ttc` 在 Android 上不存在。需要：
- Android 上使用 `/system/fonts/NotoSansCJK-Regular.ttc` 或回退到默认字体
- 通过 `#ifdef __ANDROID__` 条件编译选择字体路径

## 启动方式

```bash
# Editor 模式（默认）
adb shell am start -n com.qymengine.app/.QymActivity

# Runtime Player 模式
adb shell am start -n com.qymengine.app/.QymActivity --es args "-runtime-player"
```

QymActivity.java 将 intent extra "args" 传递给 SDL_main 的 argv。
