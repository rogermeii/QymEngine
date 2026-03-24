# QymEngine 工程 Review 报告

日期：2026-03-21

## 审查方式

- 静态代码审查
- 构建验证：`cmake --build build3 --config Debug --target QymEditor`

## 审查范围

- `CMakeLists.txt`
- `engine/`
- `editor/`
- `assets/shaders/`
- `tools/shader_compiler/`
- `README.md`

## 总体结论

工程已经从早期 Vulkan Demo 演进成了一个带编辑器的小型引擎，模块边界也初步形成了 `engine / scene / asset / editor` 的分层，这个方向是对的。

当前最主要的问题不在“功能缺失”，而在“生命周期和状态管理”：

- 编辑器层和运行时层已经开始共享很多资源，但所有权没有完全收紧
- `Scene` 选择状态、材质缓存、descriptor 分配和 shader 反射之间存在隐式约定
- 一些关键路径已经能工作，但一旦继续扩展功能，很容易出现悬空指针、缓存失效和资源不足的问题

建议优先级应当是：

1. 先修崩溃和悬空指针
2. 再修资源生命周期和 descriptor/材质更新策略
3. 最后统一 shader 工具链和文档

## 风险等级说明

- `P1`：高风险，可能导致崩溃、严重错误行为或核心路径失效
- `P2`：中风险，短期可运行，但会导致错误行为、维护困难或后续扩展受阻
- `P3`：低风险，偏设计与工程质量问题

## 详细问题

### 1. [P1] 材质 Shader 切换存在 use-after-free

**位置**

- `editor/panels/InspectorPanel.cpp:80-86`
- `editor/panels/InspectorPanel.cpp:94-189`

**问题描述**

材质面板切换 Shader 时，代码会立即调用 `assetManager.invalidateMaterial()` 清除当前材质缓存，但当前 UI 帧后续逻辑仍继续使用已经失效的 `mat` / `mutableMat` 指针。

**为什么有问题**

`invalidateMaterial()` 会销毁材质参数缓冲并从缓存中擦除 `MaterialInstance`。后续继续读取该对象，会形成典型的 use-after-free。

**影响**

- 切换 Shader 时可能直接崩溃
- 也可能表现为偶发性异常、属性面板错乱或写入非法内存

**建议**

- 切换 Shader 后本帧立刻 `return`
- 或者先记录“待切换 shader”，在下一帧统一执行重载
- 不要在 UI 代码中一边销毁缓存，一边继续使用当前缓存对象

### 2. [P1] `Scene` 选择集会保留悬空 `Node*`

**位置**

- `engine/scene/Scene.cpp:67-73`
- `engine/scene/Scene.cpp:159-176`
- `engine/scene/Scene.cpp:217-229`
- `editor/EditorApp.cpp:175-201`

**问题描述**

`Scene` 内部既保存单选节点 `m_selectedNode`，又保存多选集合 `m_selectedNodes`。当前删除节点、反序列化场景、undo/redo 恢复时，并没有完整清理或重建选择状态。

**为什么有问题**

- 删除父节点时，只移除了父节点本身，没有移除其子树中已经被销毁的后代指针
- `fromJsonString()` / `deserialize()` 重建整棵树后，没有清空 `m_selectedNodes`
- 编辑器又会直接遍历该集合执行复制、删除、重复等操作

**影响**

- 多选删除后可能留下悬空指针
- undo/redo 后可能在后续操作中随机崩溃
- 选择状态可能与实际场景树不一致

**建议**

- 删除节点时递归清理整棵子树在选择集中的所有指针
- `deserialize()` / `fromJsonString()` 时同时清空 `m_selectedNode` 和 `m_selectedNodes`
- 更稳妥的方案是把选择状态从裸指针切换为稳定 ID

### 3. [P1] 独立运行时入口已与当前渲染器设计脱节

**位置**

- `QymEngine/main.cpp:17-21`
- `engine/renderer/Renderer.cpp:136-144`
- `editor/panels/SceneViewPanel.cpp:18-38`

**问题描述**

顶层 `QymEngine` 可执行程序仍然保留“独立运行时应用”的入口，但当前渲染器主路径已经默认依赖编辑器里的 Scene View 离屏渲染。

**为什么有问题**

`Renderer::drawScene()` 只有在离屏 render pass 和 framebuffer 已创建时才会真正绘制，而离屏资源创建是由 `SceneViewPanel` 的 resize 流程驱动的。独立运行时程序没有这条路径，因此很可能只是在跑主循环，却没有真正输出场景内容。

**影响**

- 工程表面上存在两个可执行目标，但其中一个行为已经不可信
- 新成员会误以为 runtime 入口仍然有效
- 后续调试时容易把“编辑器专用设计”误当成“通用渲染器设计”

**建议**

二选一明确下来：

- 如果保留独立 runtime：恢复一条面向 swapchain 的正式渲染路径
- 如果只保留编辑器：删除或弱化该可执行目标，避免假入口继续存在

### 4. [P2] Swapchain `preTransform` 写死为 `IDENTITY`

**位置**

- `engine/renderer/SwapChain.cpp:78-80`

**问题描述**

swapchain 创建时直接使用 `VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR`，没有根据 surface capability 选择实际支持的变换。

**为什么有问题**

并不是所有 surface 都保证支持 identity transform。移动平台、旋转屏幕或某些窗口系统下，这种写法可能直接导致 `vkCreateSwapchainKHR` 失败。

**影响**

- Windows 下通常没问题
- Android/移动端适配风险较高
- 平台问题会在 swapchain 创建阶段直接暴露

**建议**

- 默认使用 `capabilities.currentTransform`
- 仅在 capability 明确支持 identity 时才使用 identity

### 5. [P2] 物理设备筛选逻辑和真实功能需求不一致

**位置**

- `engine/renderer/VulkanContext.cpp:291-295`
- `engine/renderer/VulkanContext.cpp:359-361`
- `engine/renderer/Renderer.cpp:676-677`

**问题描述**

当前设备筛选要求 `geometryShader`，但工程并未使用 geometry shader；同时设备创建启用了 `fillModeNonSolid`，但筛选阶段没有验证这一 feature 是否可用。

**为什么有问题**

- 会错误淘汰本来可以运行的设备
- 也可能放过最终无法支持线框模式的设备
- “筛选通过”和“实际能创建所有 pipeline”之间没有对齐

**影响**

- 可移植性下降
- 设备兼容性判断不可靠
- 线框选中高亮可能在部分设备上出问题

**建议**

- 删除对 `geometryShader` 的强制依赖
- 在评分和创建设备时统一检查 `fillModeNonSolid`
- 让“筛选条件”和“实际 pipeline 需求”完全一致

### 6. [P2] 材质面板修改贴图后不会立即刷新 GPU descriptor

**位置**

- `editor/panels/InspectorPanel.cpp:143-162`
- `editor/panels/InspectorPanel.cpp:168-189`
- `engine/asset/AssetManager.cpp:503-543`

**问题描述**

在 Inspector 中修改 `texture2D` 属性时，只更新了 `MaterialInstance::texturePaths` 的内存值，没有同步更新 descriptor set。点击 “Save Material” 也只是把 JSON 写回文件，并未触发材质重建。

**为什么有问题**

当前 descriptor 只在 `loadMaterial()` 首次创建材质时写入一次，之后不会自动更新。

**影响**

- Inspector 显示和实际渲染结果不一致
- 用户会看到“改了但不生效”的现象
- 贴图调试体验较差

**建议**

- 属性修改时立即重写对应 binding 的 descriptor
- 或保存后主动 `invalidateMaterial()` 并重载
- 同时考虑让材质对象具备“脏标记 + 延迟刷新”机制

### 7. [P2] 常驻映射的 UBO 在释放前没有 `vkUnmapMemory()`

**位置**

- `engine/renderer/Buffer.cpp:52`
- `engine/renderer/Buffer.cpp:56-63`

**问题描述**

uniform buffer 在创建时做了持久映射，但销毁时没有显式 `vkUnmapMemory()`。

**为什么有问题**

很多驱动能容忍这种写法，但在 validation layer 下通常会报资源生命周期错误，也会让清理逻辑显得不完整。

**影响**

- 增加 validation 噪音
- 资源管理不严谨
- 后续如果引入更复杂的映射策略，会更难统一

**建议**

- 在 `Buffer::cleanup()` 中显式 `vkUnmapMemory()`
- 清空 `m_uniformBuffersMapped`、`m_uniformBuffers`、`m_uniformBuffersMemory`

### 8. [P2] Descriptor pool 职责混杂，容量预算过于乐观

**位置**

- `engine/renderer/Descriptor.cpp:8-25`
- `engine/renderer/Renderer.cpp:57-85`
- `engine/asset/AssetManager.cpp:273-299`
- `editor/panels/ModelPreview.cpp:231-249`

**问题描述**

同一个 descriptor pool 目前同时承担了：

- 每帧 UBO set
- 材质 set
- Inspector 纹理预览 set
- Model Preview 自己的 UBO set

但 pool 的预算模型仍然只是“帧数 + 材质数”的粗略估计。

**为什么有问题**

随着资源量增加，`vkAllocateDescriptorSets` 的失败会变得不可预测，而且问题表象会和用户当前操作不一致。

**影响**

- 资源多时可能出现随机分配失败
- 编辑器功能和运行时材质会相互抢池子
- 后续加更多预览面板时风险更高

**建议**

- 至少拆分为“运行时材质池 / 编辑器预览池 / ImGui 池”
- 为不同 descriptor 类型分别做更准确的预算
- 如果后续频繁创建销毁材质，可考虑独立 allocator 或可重置子池

### 9. [P2] Shader 编译链存在两个真相源

**位置**

- `engine/renderer/Pipeline.cpp:216-222`
- `tools/shader_compiler/main.cpp:306-314`
- `assets/shaders/compile.bat:4-14`

**问题描述**

运行时依赖 `.reflect.json` 来决定 descriptor layout 和 push constant range，但当前工程里同时存在两套 shader 产物生成方式：

- `tools/shader_compiler`：生成 `.spv` 和 `.reflect.json`
- `assets/shaders/compile.bat`：只生成 `.spv`

**为什么有问题**

只要有人更新了 shader 的 `.spv` 却没有同步更新 `.reflect.json`，运行时布局就可能和真实 shader 不一致。

**影响**

- 材质绑定错位
- push constant 大小和布局错误
- descriptor binding 定义与 shader 不一致时更难排查

**建议**

- 统一只保留一条正式 shader 编译入口
- 把 reflection 生成纳入同一条命令
- 在构建系统中显式声明 `.spv` 与 `.reflect.json` 的依赖关系

### 10. [P3] 文档与当前工程实现明显漂移

**位置**

- `README.md`

**问题描述**

当前 README 仍然描述 GLFW、旧目录结构、`HelloTriangleApplication` 和早期 demo 入口，但实际工程已经迁移到了 SDL2 + CMake + engine/editor + assets/shaders 体系。

**为什么有问题**

这不会立刻导致运行错误，但会显著提高理解成本，并在构建、调试、排查问题时制造误导。

**影响**

- 新成员上手成本高
- 审查和维护需要先“纠正文档”
- 容易把历史设计当成当前事实

**建议**

- 更新 README 的技术栈、目录结构、构建步骤和 shader 工作流
- 明确说明当前正式入口是 `QymEditor`
- 说明 `assets/`、`tools/shader_compiler/` 与 `.reflect.json` 的关系

## 设计层面的总体建议

### 1. 收紧对象所有权

重点把以下几类状态从“裸指针 + 约定”升级为“显式所有权 + 明确刷新机制”：

- Scene 选择状态
- 材质缓存失效
- 预览面板引用的 GPU 资源
- runtime 和 editor 共享的 descriptor 分配

### 2. 让运行时路径和编辑器路径显式分离

现在 `Renderer` 同时承担：

- runtime 基础渲染
- 编辑器 Scene View 离屏
- 材质 fallback
- 选中高亮线框
- grid
- 模型预览依赖

建议后续拆分出更清晰的职责边界，例如：

- `Renderer` 负责 Vulkan 基础设施和帧调度
- `SceneRenderer` 负责场景绘制
- `EditorRenderFeatures` 负责 grid、gizmo、selection outline、preview

### 3. 统一 shader 资产规范

建议明确一个正式规范：

- 源文件：`.slang`
- 编译产物：`.spv`
- 反射产物：`.reflect.json`
- 元数据：`.shader.json`

并让工具链、运行时和文档都基于这套规范工作。

## 建议整改顺序

### 第一阶段：先修稳定性

1. 修复材质 Shader 切换的 use-after-free
2. 修复 `Scene` 选择集悬空指针问题
3. 补齐 UBO unmap 和其它资源清理细节

### 第二阶段：修运行时一致性

1. 修正 swapchain `preTransform`
2. 对齐 physical device feature 筛选逻辑
3. 明确 runtime 和 editor 的渲染路径边界

### 第三阶段：修工程可维护性

1. 拆分 descriptor pool 职责
2. 材质修改支持实时刷新 descriptor
3. 统一 shader 编译入口
4. 更新 README 和开发文档

## 本次审查的限制

- 本次主要是静态代码审查
- 我做了 `QymEditor` 的 Debug 构建验证，当前可以成功编译
- 没有进行交互式运行验证
- 没有进行 RenderDoc 抓帧级别的动态验证

## 附加说明

当前工程的基础方向是好的，尤其是编辑器、材质系统和反射驱动 pipeline 的演进思路都值得继续做下去。问题主要集中在“系统已经长大了，但底层状态管理还保留了很多 demo 阶段写法”。这类问题越早收拾，后面加功能越顺。
