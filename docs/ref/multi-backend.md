# 多后端渲染架构

引擎支持 Vulkan / D3D12 / D3D11 / OpenGL / GLES 五个后端。

## PC 后端启动参数

```bash
QymEditor.exe              # 默认 Vulkan
QymEditor.exe --opengl     # OpenGL 4.5
QymEditor.exe --gles       # GLES 3.0 模拟（Desktop GL）
QymEditor.exe --d3d12      # D3D12
QymEditor.exe --d3d11      # D3D11
```

## 矩阵上传约定

C++ 侧所有矩阵（view, proj, lightVP）在存入 UBO 前做 `glm::transpose()`，以适配 Slang SPIR-V 的行主序解读。

**Push Constants 中的 model 矩阵例外**：通过 `toShaderMatrix()` 处理：
- Vulkan / D3D: `glm::transpose(m)` — 与 UBO 中其他矩阵一致
- OpenGL / GLES: `m`（原始列主序）— 因为 GLSL fixup 已处理乘法顺序

## OpenGL GLSL fixup 机制

文件: `engine/renderer/opengl/VkOpenGL.cpp` 中的 `fixupGLSL()` 函数。

Slang 编译 GLSL 时生成 `v * unpackStorage(M)` 形式的行向量乘法。`fixupGLSL` 通过**精确字符串匹配**做两步修正：
1. 将 `unpackStorage` 简化为恒等函数（移除内部转置）
2. 将 `v * M` 形式改写为 `transpose(M) * v`（加显式 transpose 并改为列向量乘法）

**已知脆弱性**：Slang 可能内联中间变量，导致生成的 GLSL 表达式与预置的匹配模式不符。修改 `.slang` 着色器后需检查 fixup 模式是否仍然覆盖所有路径。

## 阴影系统跨后端差异

`FrameData.shadowParams` (ivec4) 控制各后端差异：
- `.x` — 阴影启用标志
- `.y` — D3D UV Y 翻转（D3D viewport Y-up，shadow map UV 需要翻转）
- `.z` — GLES NDC 深度转换（[-1,1] → [0,1]，仅无 glClipControl 时）

光源投影矩阵 `lightProj` 的后端差异：
- Vulkan: `lightProj[1][1] *= -1`（Y-flip）
- D3D / OpenGL / GLES: 无 Y-flip
- GLES 无 glClipControl: 额外 `depthFix` 矩阵将 [0,1] 映射到 [-1,1]

## GLES 特殊限制

- 无 `glTextureView` → bloom 等 per-mip 操作通过 `GL_TEXTURE_BASE_LEVEL/MAX_LEVEL` 实现
- 无 `glClipControl` → NDC 深度 [-1,1]，需要 depthFix 矩阵和着色器端转换
- 阴影使用 `R32F` 颜色纹理存深度（而非 `D32F` 深度纹理）
