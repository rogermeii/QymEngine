# QymEngine

基于 Vulkan 的 3D 游戏引擎，包含编辑器和独立运行时。

## 技术栈

- **语言**: C++17 | **图形 API**: Vulkan 1.4 | **窗口**: SDL2 | **着色器**: Slang
- **构建**: CMake | **平台**: Windows x64, Android
- **后端**: Vulkan / D3D12 / D3D11 / OpenGL / GLES

## 构建（Windows）

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --config Debug
```

产物: `build3/editor/Debug/QymEditor.exe`, `build3/Debug/QymEngine.exe`

## 运行

```bash
QymEditor.exe              # Vulkan（默认）
QymEditor.exe --opengl     # OpenGL
QymEditor.exe --gles       # GLES
QymEditor.exe --d3d12      # D3D12
QymEditor.exe --d3d11      # D3D11
```

## 编码规范

- 所有注释和文档使用中文
- 引擎代码位于 `QymEngine` 命名空间
- 修改渲染代码时必须考虑对所有后端的影响

## 按需参考文档

涉及特定领域时读取对应文档：

| 场景 | 文档 |
|------|------|
| Android 构建/部署/调试 | `docs/ref/android.md` |
| 多后端渲染/矩阵约定/GLSL fixup/阴影 | `docs/ref/multi-backend.md` |
| 着色器编译/材质系统/描述符集 | `docs/ref/shader-system.md` |
