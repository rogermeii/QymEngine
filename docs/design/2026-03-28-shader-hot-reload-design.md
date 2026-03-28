# Shader 热重载设计

## 背景

当前修改 .slang 着色器后需要手动重编并重启引擎才能看到效果。`Renderer::reloadShaders()` 已实现全量重编+重建 pipeline 的逻辑，但缺少自动触发和增量编译能力。

## 设计

### 1. 文件监视（ShaderFileWatcher）

新建 `engine/asset/ShaderFileWatcher.h/cpp`。

- 后台线程每 500ms 轮询 `assets/shaders/` 下所有 `.slang` 文件的 `std::filesystem::last_write_time`
- 与上次记录的时间戳对比，检测到变化时记录修改文件列表
- 主线程通过 `pollChanges()` 查询变化列表，触发重载
- `#ifdef __ANDROID__` 禁用（APK assets 只读）
- 线程安全：修改文件列表用 `std::mutex` 保护

### 2. 依赖追踪

ShaderFileWatcher 首次扫描时解析所有 `.slang` 文件：
- 用正则提取 `#include "xxx"` 和 `import xxx` 语句
- 构建反向依赖表：`map<string, vector<string>>`（被依赖文件 → 依赖它的文件列表）
- 修改一个文件时，所有依赖它的文件也加入重编列表
- 依赖表在文件变化时惰性更新（重新解析变化的文件）

### 3. 增量编译

不修改 ShaderCompiler，在引擎侧实现增量：
- 对每个需要重编的 .slang 文件，单独调用 `ShaderCompiler <file> <output_dir> --no-msl`
- 只重建对应的 .shaderbundle 文件
- 编译失败时打印错误日志但不中断（保留旧 pipeline 继续运行）

### 4. 增量 pipeline 重载

新增 `Renderer::reloadModifiedShaders(changedBundles)` 方法：

| Bundle 文件 | 对应 Pipeline | 重载方式 |
|------------|--------------|---------|
| Lit/Triangle/Unlit | 场景材质管线 | AssetManager::invalidateShader(path) + 重建 pipeline |
| Shadow | m_shadowVkPipeline | destroyShadowResources() + createShadowResources() |
| Sky | m_skyPipeline | 重建 fullscreen pipeline |
| Grid | m_gridPipeline | 重建 fullscreen pipeline |
| BloomDownsample/BloomUpsample/DOF/Composite/FXAA | 后处理管线 | PostProcess::reloadShader(name) |
| IBL 相关 | IBL 管线 | 不参与热重载（一次性生成） |

流程：
1. `vkDeviceWaitIdle()` — GPU 同步
2. 遍历 changedBundles，按文件名匹配需要重建的 pipeline
3. 只 cleanup + recreate 受影响的 pipeline
4. 未受影响的 pipeline 保持不变

### 5. 触发方式

- **Ctrl+R**：全量重载（重编所有 shader，重建所有 pipeline）
- **文件监视**：增量重载（只重编修改的文件，只重建受影响的 pipeline）

### 6. UI 反馈

- 热重载开始时在 scene view 叠加显示 "Reloading shaders..."
- 完成后显示 "Shaders reloaded (N files, Xms)"，持续 2 秒后消失
- 编译失败时显示红色错误信息

## 涉及文件

| 文件 | 改动 |
|------|------|
| `engine/asset/ShaderFileWatcher.h/cpp` | 新建：文件监视 + 依赖追踪 |
| `engine/renderer/Renderer.h/cpp` | 新增 reloadModifiedShaders()，集成 ShaderFileWatcher |
| `engine/renderer/PostProcess.h/cpp` | 新增 reloadShader(name) 单个管线重载 |
| `editor/EditorApp.cpp` | Ctrl+R 快捷键 + UI 反馈显示 |

## 不做

- 不修改 ShaderCompiler（增量编译通过多次单文件调用实现）
- IBL 管线不参与热重载
- Android 不支持热重载（APK 只读）
