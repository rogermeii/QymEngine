# GLSL Fixup 矩阵修正消除 + Android 构建流程简化

## 背景

### GLSL Fixup 问题
OpenGL/GLES 后端的 `fixupGLSL` 函数中有 28 条 `replaceAll` 精确字符串匹配规则，用于修正 Slang 编译器生成的 GLSL 中的矩阵乘法顺序。每次修改 `.slang` 着色器，Slang 可能生成不同格式的 GLSL 代码（如内联变量），导致匹配模式失效、渲染错误。

根因：C++ 侧对 UBO 中的矩阵做了 `glm::transpose()`（view、proj、lightVP），但 Slang GLSL 输出的 `unpackStorage` 函数又做了一次转置，导致需要 fixup 规则来补偿。

### Android 构建问题
`gradle assembleDebug` 依赖 `buildHostShaderCompiler` task，该 task 调用 `cmake --build build3 --target ShaderCompiler`，在 Visual Studio 生成器下参数传递失败。实际工作流中开发者一定先在 PC 编译引擎（ShaderCompiler 已自动构建），无需 Gradle 重复编译。

## Part 1: GLSL Fixup 矩阵修正消除

### 方案
OpenGL/GLES 后端的 UBO 矩阵不再做 `glm::transpose()`，让 Slang 原始生成的 GLSL 代码直接正确。

### 原理
当前数据流：
```
C++ 侧: ubo.view = glm::transpose(view)
  → UBO 内存: transpose(view) 的列主序布局
  → GLSL std140 读取: 得到 transpose(view) 作为 mat4
  → unpackStorage() 内部转置: 得到 view
  → Slang 生成 v * view: 等价于 view^T * v = transpose(view) * v ← 错误
  → fixup 规则修正: transpose(view) * v → view * v ← 正确
```

修改后数据流：
```
C++ 侧: ubo.view = view（不转置）
  → UBO 内存: view 的列主序布局
  → GLSL std140 读取: 得到 view 作为 mat4
  → unpackStorage() 内部转置: 得到 transpose(view)
  → Slang 生成 v * transpose(view): 等价于 view * v ← 直接正确
```

### 改动点
1. **`Renderer.cpp` - `updateUniformBuffer`**：OpenGL/GLES 时 view、proj、lightVP 不调用 `glm::transpose()`
2. **`VkOpenGL.cpp` - `fixupGLSL`**：删除全部 28 条矩阵相关 replaceAll 规则
3. **保留**：fixupGLSL 中的 sampler rebinding、GLES 兼容性处理（std430→std140、push_constant 移除、#line 清理、flat 修饰符等）

### 影响范围
- 仅影响 OpenGL/GLES 后端
- Vulkan/D3D 后端不受影响（它们继续使用 transpose）
- 所有 6 个使用矩阵的着色器受益（Lit、Shadow、Sky、Grid、Triangle、Unlit）

## Part 2: Android 构建流程简化

### 方案
Gradle 不再编译 ShaderCompiler，假设它已由 PC cmake 构建好。

### 改动点
1. **删除** `configureHostTools` 和 `buildHostShaderCompiler` 两个 Exec task
2. **`compileShaderBundles`**：检查 ShaderCompiler 是否存在，存在则编译 shader，不存在则跳过并打印警告
3. **`syncExternalAssets`**：不再依赖 `compileShaderBundles`，改为独立 task
4. **`preBuild`**：依赖 `syncExternalAssets`（必须）和 `compileShaderBundles`（可选）

### 构建流程
```
开发者先在 PC 编译引擎（cmake --build . → ShaderCompiler 自动构建 + CompileShaders 自动编译所有 .slang）
  ↓
gradle assembleDebug
  ├─ compileShaderBundles（找到 ShaderCompiler 则编译，否则跳过）
  ├─ syncExternalAssets（同步 assets/ 到 APK，含已有的 .shaderbundle）
  └─ NDK 编译 + APK 打包
```

## 验证计划
- PC: Vulkan / OpenGL / GLES / D3D11 / D3D12 全端验证
- Android: Vulkan / GLES 验证
- 重点关注阴影、法线变换、天空盒、网格渲染的正确性
