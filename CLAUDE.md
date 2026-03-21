# QymEngine

基于 Vulkan 的 3D 游戏引擎，包含编辑器和独立运行时。

## 技术栈

- **语言**: C++17
- **图形 API**: Vulkan 1.4
- **窗口系统**: SDL2 (FetchContent 获取)
- **着色器语言**: Slang (.slang → .spv，通过离线 ShaderCompiler 编译)
- **构建系统**: CMake (可用 Visual Studio 2022 打开 CMakeLists.txt 或使用 cmake 生成 .sln)
- **平台**: Windows x64, Android

## 项目结构

```
QymEngine/
├── engine/                    # 引擎静态库 (QymEngineLib)
│   ├── core/                  # Application, Window 基类
│   ├── renderer/              # Vulkan 渲染器 (Pipeline, SwapChain, Descriptor 等)
│   ├── scene/                 # Scene, Node, Camera
│   └── asset/                 # AssetManager (模型/纹理/材质加载)
├── editor/                    # 编辑器可执行文件 (QymEditor)
│   ├── EditorApp.cpp/h        # 编辑器主程序
│   ├── ImGuiLayer.cpp/h       # ImGui 集成
│   └── panels/                # Inspector, ProjectPanel, SceneView, ModelPreview 等
├── QymEngine/
│   └── main.cpp               # 独立运行时可执行文件 (QymEngine)
├── assets/
│   ├── shaders/               # Slang 着色器 + 编译产物 (.spv) + 反射 (.reflect.json)
│   ├── materials/             # 材质定义 (.mat.json)
│   ├── models/                # 3D 模型
│   ├── textures/              # 纹理资源
│   └── scenes/                # 场景文件 (.json)
├── tools/
│   └── shader_compiler/       # Slang → SPIR-V 离线编译器
├── android-cmake/             # Android 运行时入口
├── android/                   # Android Gradle 工程
├── 3rd-party/                 # 第三方库
│   ├── glm-1.0.1-light/       # 数学库 (header-only)
│   ├── stb-master/            # 图像加载 (header-only)
│   ├── imgui/                 # ImGui (docking 分支)
│   ├── imguizmo/              # ImGuizmo (Gizmo 操作)
│   ├── nlohmann/              # JSON 库 (header-only)
│   └── tinyobjloader/         # OBJ 模型加载 (header-only)
└── CMakeLists.txt
```

## 构建

```bash
cd build3
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --config Debug
```

- 需要安装 Vulkan SDK
- 链接库: SDL2 (静态), `vulkan-1.lib`
- 产物: `QymEditor.exe` (编辑器), `QymEngine.exe` (独立运行时)

## 着色器系统

着色器使用 Slang 语言编写 (`.slang`)，通过 `tools/shader_compiler/` 离线编译为 SPIR-V (`.spv`)。
编译同时生成反射信息 (`.reflect.json`)，供材质系统使用。

着色器定义文件: `.shader.json` (引用 .spv 和 .reflect.json)

## 材质系统

- **着色器定义**: `.shader.json` — 引用顶点/片段 .spv 和反射信息
- **材质实例**: `.mat.json` — 引用 shader.json，设置具体参数和纹理
- **反射信息**: `.reflect.json` — 着色器编译时自动生成的 binding 布局

## 描述符集设计

- **Set 0**: Per-frame 数据 (FrameData UBO — VP 矩阵等)
- **Set 1**: Per-material 数据 (着色器特定参数 + 纹理)
- **Push Constants**: Per-object 数据 (Model 矩阵)

## 编码规范

- 所有注释和文档使用中文
- 引擎代码位于 `QymEngine` 命名空间
