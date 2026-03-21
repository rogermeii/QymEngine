# QymEngine

基于 Vulkan 的图形渲染引擎/Demo 项目。

## 技术栈

- **语言**: C++17
- **图形 API**: Vulkan 1.4
- **构建系统**: Visual Studio 2022 (MSBuild, v143 工具集)
- **平台**: Windows x64

## 项目结构

```
QymEngine/
├── QymEngine/
│   ├── main.cpp              # 主程序 (Vulkan 渲染应用)
│   ├── shaders/
│   │   ├── Triangle.vert     # 顶点着色器 (GLSL 4.50)
│   │   ├── Triangle.frag     # 片段着色器 (GLSL 4.50)
│   │   └── compile.bat       # 着色器编译脚本 (GLSL → SPIR-V)
│   └── textures/
│       └── texture.jpg
├── 3rd-party/
│   ├── glfw-3.4.bin.WIN64/   # 窗口管理
│   ├── glm-1.0.1-light/      # 数学库 (header-only)
│   └── stb-master/           # 图像加载 (header-only)
└── QymEngine.sln
```

## 构建

- 使用 Visual Studio 2022 打开 `QymEngine.sln`
- 需要安装 Vulkan SDK (路径: `C:\VulkanSDK\1.4.309.0`)
- 链接库: `glfw3.lib`, `vulkan-1.lib`

## 着色器编译

```bat
cd QymEngine/shaders
compile.bat    # 使用 glslc 编译 GLSL → SPIR-V (.spv)
```

## 编码规范

- 所有注释和文档使用中文
- 主类: `HelloTriangleApplication`，包含完整的 Vulkan 初始化和渲染管线
