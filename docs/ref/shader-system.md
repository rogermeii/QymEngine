# 着色器与材质系统

## 着色器编译流程

`.slang` → ShaderCompiler → `.shaderbundle`（包含多变体：SPIR-V / DXIL / DXBC / GLSL）

```bash
# 编译单个着色器
build3/tools/shader_compiler/Debug/ShaderCompiler.exe assets/shaders/Lit.slang

# 编译选项
--no-glsl     # 不生成 GLSL 变体
--no-dxil     # 不生成 DXIL 变体
--no-dxbc     # 不生成 DXBC 变体
--debug       # 生成调试符号
```

## Shaderbundle 格式

二进制文件，包含多个编译变体：
- `default` — SPIR-V（Vulkan 后端）
- `default_glsl` — GLSL 450（OpenGL/GLES 后端）
- `default_dxil` — DXIL SM6.0（D3D12 后端）
- `default_dxbc` — DXBC SM5.0（D3D11 后端）
- `bindless` — Bindless 变体（各格式）

每个变体包含 VS/PS 二进制 + 反射 JSON。

## 材质系统

- **着色器定义** `.shader.json` — 引用 .shaderbundle 和反射信息
- **材质实例** `.mat.json` — 引用 shader.json，设置具体参数和纹理
- **反射信息** `.reflect.json` — 着色器编译时自动生成的 binding 布局

## 描述符集设计

- **Set 0**: Per-frame 数据（FrameData UBO — VP 矩阵、光源、阴影矩阵、IBL 贴图）
- **Set 1**: Per-material 数据（着色器特定参数 + 纹理）
- **Push Constants**: Per-object 数据（Model 矩阵、highlighted 标记）

## Slang 编译器矩阵约定

- SPIR-V 变体: Slang 默认行主序（`SLANG_MATRIX_LAYOUT_ROW_MAJOR`）
- GLSL 变体: 显式设为列主序（`SLANG_MATRIX_LAYOUT_COLUMN_MAJOR`），配合 `fixupGLSL` 修正

Slang 生成 GLSL 时使用 `_MatrixStorage` 包装和 `unpackStorage` 函数，`fixupGLSL` 将其简化为直接 `mat4` 操作。
