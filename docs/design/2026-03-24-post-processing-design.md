# 后处理系统设计

## 概述

为 QymEngine 实现完整的后处理管线，包含 Bloom、Tone Mapping、Color Grading 和 FXAA 四个效果。采用合并 Pass 策略，将 Tone Mapping + Color Grading 合并到单个 Composite Pass，Bloom 和 FXAA 因技术限制需独立 Pass。

## 需求

- HDR 渲染：offscreen 升级为 R16G16B16A16_SFLOAT
- 效果链：Bloom → Composite（Tone Mapping + Color Grading）→ FXAA
- 参数化 Color Grading（对比度/饱和度/色温/色调/亮度），Inspector 可调
- FXAA 先行，TAA 后续扩展
- Slang 着色器，仅 Vulkan 后端
- 不做 Volume 系统

## 整体管线

```
Shadow → Scene (HDR R16G16B16A16_SFLOAT)
       → Bloom Bright Extract (降采样到半分辨率)
       → Bloom Downsample chain (逐级降采样 + 模糊)
       → Bloom Upsample chain (逐级升采样 + 混合)
       → Composite Pass (Bloom合成 + ToneMapping + ColorGrading，输出 LDR)
       → FXAA Pass (抗锯齿，输出 LDR)
       → Blit to Swapchain
       → ImGui → Present
```

## HDR 格式升级的连锁影响

将 offscreen 从 `R8G8B8A8_SRGB` 升级为 `R16G16B16A16_SFLOAT` 需要以下连锁修改：

1. **m_offscreenImage** — 创建时 format 改为 `VK_FORMAT_R16G16B16A16_SFLOAT`
2. **m_offscreenRenderPass** — attachment description 的 format 改为 HDR 格式，需要重建
3. **受影响的 Pipeline 全部重建**（因为 RenderPass 兼容性要求）：
   - `m_offscreenPipeline` / `m_bindlessOffscreenPipeline`（主场景）
   - `m_wireframePipeline` / `m_bindlessWireframePipeline`（线框）
   - `m_skyPipeline`（天空）
   - `m_gridPipeline`（网格）
   - `m_shadowVkPipeline` 不受影响（独立 RenderPass）
4. **Blit 兼容性** — `vkCmdBlitImage` 从 R16F HDR 到 R8G8B8A8_SRGB swapchain：
   - Vulkan spec 允许不同格式间 blit（需设备支持 `linearTilingFeatures` 的 `BLIT_SRC`/`BLIT_DST`）
   - 但后处理启用后，blit 源改为 `m_fxaaImage`（LDR），不存在 HDR→LDR blit 问题
   - 后处理全部关闭时需要 fallback 路径（见"效果开关行为"一节）
5. **R16G16B16A16_SFLOAT 设备支持** — Vulkan 1.0 spec 要求此格式必须支持 SAMPLED_IMAGE 和 COLOR_ATTACHMENT，桌面 GPU 普遍支持。init 时添加 `vkGetPhysicalDeviceFormatProperties` 断言检查。

## 资源（Render Target）规划

| RT 名称 | 格式 | 分辨率 | 用途 |
|---------|------|--------|------|
| m_offscreenImage | R16G16B16A16_SFLOAT | 全分辨率 | 场景 HDR 渲染结果（已有，改格式） |
| m_offscreenDepthImage | D32_SFLOAT | 全分辨率 | 深度（已有，不变） |
| m_bloomMipChain | R16G16B16A16_SFLOAT | 1/2 → 1/2^N | Bloom 降采样/升采样 mip 链（单张纹理多 mip） |
| m_compositeImage | R8G8B8A8_UNORM | 全分辨率 | Composite 输出（LDR，手动 gamma） |
| m_fxaaImage | R8G8B8A8_UNORM | 全分辨率 | FXAA 输出 → 最终 blit 源 |

**注意**：Composite 和 FXAA 的输出格式使用 `R8G8B8A8_UNORM` 而非 `_SRGB`。原因：FXAA 需要在 gamma 空间（感知亮度）下计算边缘检测，如果使用 SRGB 格式，硬件采样时自动解码为 linear，FXAA 亮度计算不准确。因此 Composite shader 在最后一步手动做 linear → sRGB gamma 矫正，输出到 UNORM 格式，FXAA 直接在 gamma 空间工作。

### Bloom Mip Chain

- 使用单张纹理的多个 mip level，减少资源管理复杂度
- mip 0 = 1/2 分辨率（bright extract 输出），mip 1 = 1/4，... 最多 5-6 级
- Downsample 阶段：mip N → mip N+1（逐级缩小 + 模糊）
- Upsample 阶段：mip N+1 → mip N（逐级放大 + 与上一级叠加）
- 最终 Bloom 结果在 mip 0
- **Mip 级数上限**：编译期常量 `MAX_BLOOM_MIPS = 6`，运行时 `bloomMipCount` 只允许在 [1, MAX_BLOOM_MIPS] 范围内调节
- **资源创建策略**：bloom mip chain image 始终创建 `MAX_BLOOM_MIPS` 级 mip，`bloomMipCount` 仅控制运行时实际使用的级数，不触发资源重建
- **Image Usage Flags**：`VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`（同一张 image 的不同 mip 交替作为 attachment 写入和 shader 采样读取）

### 窗口 Resize

offscreen、compositeImage、fxaaImage、bloomMipChain 全部销毁重建（与现有 `resizeOffscreen` 流程一致）。

## Bloom 多 Pass 流程

共 2N+1 个 pass，N = mip 级数，建议 N=5。

### Step 1: Bright Extract + Downsample to Mip 0

- 输入：m_offscreenImage (HDR 全分辨率)
- 输出：m_bloomMipChain mip 0 (1/2 分辨率)
- 操作：采样 HDR → 提取亮度 > threshold 的部分 → 写入 mip 0
- 着色器：bloom_downsample.slang（含亮度阈值逻辑，仅首次 dispatch 启用）

### Step 2: Downsample Chain (mip 0 → mip 4)

- 循环 i = 0..3
- 输入：mip i → 输出：mip i+1
- 操作：13-tap downsample filter（参考 CoD 2014 的 dual filtering），避免 firefly artifacts
- 着色器：同 bloom_downsample.slang（关闭阈值，纯降采样模糊）

### Step 3: Upsample Chain (mip 4 → mip 0)

- 循环 i = 3..0
- 输入：mip i+1 → 输出：mip i（叠加到已有内容上）
- 操作：tent filter 升采样 + 与当前 mip 线性混合
- 着色器：bloom_upsample.slang
- 混合模式：Additive blend（VK_BLEND_OP_ADD, src=ONE, dst=ONE）

### Bloom Mip Layout Transition 时序

每个 bloom mip 在处理过程中经历以下 layout 状态转换（通过 `vkCmdPipelineBarrier` 指定 `baseMipLevel` 和 `levelCount=1`）：

**Downsample 阶段：**
```
初始状态: 所有 mip = UNDEFINED

Bright Extract (offscreen → mip 0):
  offscreen: SHADER_READ_ONLY (已就绪) → 保持
  mip 0: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL (写入)
  写入完成后:
  mip 0: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL (下一步读取)

Downsample mip 0 → mip 1:
  mip 0: SHADER_READ_ONLY_OPTIMAL (读取)
  mip 1: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL (写入)
  写入完成后:
  mip 1: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL

... 以此类推到 mip N-1 → mip N
```

**Upsample 阶段：**
```
Upsample mip N → mip N-1:
  mip N: SHADER_READ_ONLY_OPTIMAL (读取，downsample 完成后已就绪)
  mip N-1: SHADER_READ_ONLY_OPTIMAL → COLOR_ATTACHMENT_OPTIMAL (需要 LOAD 保留内容)
  RenderPass LoadOp = VK_ATTACHMENT_LOAD_OP_LOAD (保留 downsample 写入的数据)
  Additive blend 将上采样结果叠加到已有内容上
  写入完成后:
  mip N-1: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL

... 以此类推到 mip 1 → mip 0

最终: mip 0 = SHADER_READ_ONLY_OPTIMAL，包含完整 Bloom 信息，供 Composite 采样
```

**关键点**：
- Upsample RenderPass 的 LoadOp 必须为 `VK_ATTACHMENT_LOAD_OP_LOAD`，保留 downsample 阶段的数据
- Downsample RenderPass 的 LoadOp 为 `VK_ATTACHMENT_LOAD_OP_DONT_CARE`（全屏覆写）
- 两个 RenderPass 实例：`m_bloomDownsampleRenderPass`（LoadOp=DONT_CARE）和 `m_bloomUpsampleRenderPass`（LoadOp=LOAD）

### Bloom Pass Vulkan 实现

- 每个 mip level 创建单独的 VkImageView（指定 baseMipLevel, levelCount=1）
- Downsample 和 Upsample 使用不同的 VkRenderPass（LoadOp 不同）
- 每个 mip 需要两个 VkFramebuffer（downsample 用 + upsample 用，绑定不同 RenderPass）
- 用全屏三角形绘制（与现有 sky/grid pass 一致）
- pass 之间通过 pipeline barrier 同步，指定精确的 mip level 范围

### Bloom Push Constant

```cpp
struct BloomPushConstant {  // 布局遵循 scalar/std430 对齐
    vec2 texelSize;      // offset 0,  当前 mip 的 1/width, 1/height
    float threshold;     // offset 8,  亮度阈值（仅 bright extract 使用）
    float intensity;     // offset 12, Bloom 强度
    int mipLevel;        // offset 16, 当前处理的 mip 级别
    int useBrightPass;   // offset 20, 是否启用亮度提取（首次 pass = 1）
};                       // 总计 24 字节
```

## Composite Pass

单个全屏 pass 完成 Bloom 合成 + Tone Mapping + Color Grading。

- 输入：m_offscreenImage (HDR 场景) + m_bloomMipChain mip 0 (Bloom 结果)
- 输出：m_compositeImage (LDR R8G8B8A8_UNORM)

着色器内执行顺序：
1. 采样 HDR 场景色
2. 如果 bloomEnabled：采样 Bloom 纹理，按 bloomIntensity 加权叠加
3. 乘以 exposure
4. 如果 toneMappingEnabled：ACES Filmic Tone Mapping（HDR → LDR）；否则直接 clamp
5. 如果 colorGradingEnabled：adjustContrast → adjustSaturation → adjustTemperature → brightness
6. **手动 linear → sRGB gamma 矫正**（因为输出格式是 UNORM 而非 SRGB）
7. clamp(0, 1) 输出

## FXAA Pass

独立全屏 pass，在 gamma 空间（UNORM）下工作，确保边缘检测基于感知亮度。

- 输入：m_compositeImage (LDR, gamma 空间)
- 输出：m_fxaaImage (LDR, gamma 空间)
- 算法：FXAA 3.11 Quality
  1. 计算当前像素及上下左右邻居的亮度 (dot(rgb, luma_weights))
  2. 对比度检测 → 跳过低对比度区域
  3. 沿梯度方向搜索边缘端点
  4. 根据边缘位置做子像素偏移采样

### FXAA Push Constant

```cpp
struct FxaaPushConstant {    // 布局遵循 scalar/std430 对齐
    vec2 texelSize;          // offset 0,  1/width, 1/height
    float subpixQuality;     // offset 8,  子像素质量（默认 0.75）
    float edgeThreshold;     // offset 12, 边缘阈值（默认 0.166）
    float edgeThresholdMin;  // offset 16, 最小阈值（默认 0.0833）
};                           // 总计 20 字节
```

## 效果开关行为

各效果开关组合的行为明确定义：

| bloomEnabled | toneMappingEnabled | colorGradingEnabled | fxaaEnabled | 行为 |
|:---:|:---:|:---:|:---:|------|
| 任意 | 任意 | 任意 | 任意 | **Composite pass 始终执行**（至少完成 HDR→LDR 转换 + gamma 矫正） |
| off | - | - | - | Composite 不采样 bloom texture，只做 tone mapping + color grading |
| - | off | - | - | 跳过 ACES，直接 clamp(exposure * color, 0, 1)，可能过曝（用户自行调 exposure） |
| - | - | off | - | 跳过 contrast/saturation/temperature 调节 |
| - | - | - | off | 跳过 FXAA pass，blit 源改为 m_compositeImage |
| off | off | off | off | Composite 仍执行（exposure + clamp + gamma），FXAA 跳过，blit 从 compositeImage |

**关键原则**：Composite pass 永远不可跳过，因为它负责 HDR→LDR 转换和 gamma 矫正。没有这一步，HDR offscreen 无法正确显示到 UNORM/SRGB swapchain。

## PostProcess 参数

```cpp
static constexpr int MAX_BLOOM_MIPS = 6;

struct PostProcessSettings {
    // Bloom
    bool  bloomEnabled = true;
    float bloomThreshold = 1.0f;     // HDR 亮度提取阈值
    float bloomIntensity = 0.5f;     // Bloom 混合强度
    int   bloomMipCount = 5;         // Bloom mip 级数 [1, MAX_BLOOM_MIPS]

    // Tone Mapping
    bool  toneMappingEnabled = true;
    float exposure = 1.0f;           // 曝光值

    // Color Grading
    bool  colorGradingEnabled = true;
    float contrast = 1.0f;           // 对比度 [0.5, 2.0]
    float saturation = 1.0f;         // 饱和度 [0.0, 2.0]
    float temperature = 0.0f;        // 色温偏移 [-1.0, 1.0]
    float tint = 0.0f;               // 色调偏移 [-1.0, 1.0]
    float brightness = 0.0f;         // 亮度偏移 [-1.0, 1.0]

    // FXAA
    bool  fxaaEnabled = true;
    float fxaaSubpixQuality = 0.75f;
    float fxaaEdgeThreshold = 0.166f;
    float fxaaEdgeThresholdMin = 0.0833f;
};
```

### 数据所有权与序列化

- `PostProcessSettings` 存放在 `Scene` 类中（场景级数据，与渲染器解耦）
- `Renderer` 通过 `drawScene(scene)` 时从 `scene.postProcessSettings` 读取
- 序列化：在 `Scene::serialize()` / `Scene::deserialize()` 中增加 `"postProcess"` JSON 节点
- Inspector 面板通过 `Scene::getPostProcessSettings()` 引用读写

## 着色器文件清单

```
assets/shaders/postprocess/
├── bloom_downsample.shaderbundle/
│   ├── bloom_downsample.slang        # fullscreen VS + bright extract/downsample FS
│   └── (编译产物: .vert.spv, .frag.spv, .reflect.json)
├── bloom_upsample.shaderbundle/
│   ├── bloom_upsample.slang          # fullscreen VS + upsample FS
│   └── (编译产物)
├── composite.shaderbundle/
│   ├── composite.slang               # fullscreen VS + composite FS
│   └── (编译产物)
└── fxaa.shaderbundle/
    ├── fxaa.slang                    # fullscreen VS + FXAA FS
    └── (编译产物)
```

每个 .shaderbundle 包含完整的顶点+片段着色器。全屏三角形顶点着色器逻辑在每个 .slang 文件的 vertex entry 中重复（代码简单，3 行，不值得引入共享机制）。

### 描述符集设计

所有后处理 pass 使用同一个 descriptor set layout：
- set 0 binding 0: sampler2D（主输入纹理）
- set 0 binding 1: sampler2D（可选，Composite pass 用于 Bloom 纹理）

参数全部通过 push constant 传递，不需要 UBO。

### Descriptor Pool

后处理系统创建独立的 `VkDescriptorPool`（与主渲染器的 pool 分离）：
- maxSets = 16（Bloom mip 数 × 2 + Composite + FXAA + 余量）
- poolSize: combinedImageSampler × 32
- 在 `PostProcessPipeline::init()` 中创建，`destroy()` 中销毁

## 代码结构

### 新增文件

```
engine/renderer/PostProcess.h/cpp     # PostProcessSettings + PostProcessPipeline 类
editor/panels/PostProcessPanel.h/cpp  # Inspector 面板
assets/shaders/postprocess/*.slang    # 后处理着色器（4 个 shaderbundle）
```

### 后处理管线的创建方式

现有 `Pipeline` 类硬编码了顶点输入布局（`Vertex::getBindingDescription()`），不支持无顶点输入的全屏 pass。后处理管线采用与现有 sky/grid 一致的方式：

- 使用裸 `VkPipeline` + `VkPipelineLayout` 成员（非 `Pipeline` 对象）
- 复用并扩展 `createFullscreenBundlePipeline()` 辅助函数，增加 push constant range 和 blend state 参数支持
- 函数签名变更：`createFullscreenBundlePipeline(..., const VkPushConstantRange* pushConstantRange = nullptr, const VkPipelineColorBlendAttachmentState* blendState = nullptr)`
- 默认 blend state 为禁用混合（大多数后处理 pass），bloom upsample 传入 additive blend（src=ONE, dst=ONE）

### PostProcessPipeline 类

```cpp
class PostProcessPipeline {
public:
    void init(VulkanContext& ctx, DescriptorLayoutCache& layoutCache,
              uint32_t width, uint32_t height);
    void destroy();
    void resize(uint32_t width, uint32_t height);
    void reloadShaders();  // 热重载时重建管线

    // 前置条件：sceneHDR 对应的 image 必须处于 SHADER_READ_ONLY_OPTIMAL layout
    // （drawSceneToOffscreen 完成后 offscreen RenderPass 的 finalLayout 保证此条件）
    void execute(VkCommandBuffer cmd,
                 VkImageView sceneHDR,
                 const PostProcessSettings& settings);

    // 根据 FXAA 开关返回最终输出
    VkImage     getFinalImage(const PostProcessSettings& settings) const;
    VkImageView getFinalImageView(const PostProcessSettings& settings) const;

private:
    void executeBloom(VkCommandBuffer cmd, VkImageView sceneHDR,
                      const PostProcessSettings& settings);
    void executeComposite(VkCommandBuffer cmd, VkImageView sceneHDR,
                          VkImageView bloomTexture,
                          const PostProcessSettings& settings);
    void executeFxaa(VkCommandBuffer cmd, const PostProcessSettings& settings);

    void createBloomResources();
    void destroyBloomResources();

    // Bloom 资源
    static constexpr int MAX_BLOOM_MIPS = 6;
    VkImage        m_bloomMipImage;
    VkDeviceMemory m_bloomMipMemory;
    VkImageView    m_bloomMipViews[MAX_BLOOM_MIPS];
    VkFramebuffer  m_bloomDownsampleFBs[MAX_BLOOM_MIPS];  // downsample 用 (LoadOp=DONT_CARE)
    VkFramebuffer  m_bloomUpsampleFBs[MAX_BLOOM_MIPS];    // upsample 用 (LoadOp=LOAD)

    // Composite 资源
    VkImage        m_compositeImage;
    VkDeviceMemory m_compositeMemory;
    VkImageView    m_compositeImageView;
    VkFramebuffer  m_compositeFramebuffer;

    // FXAA 资源
    VkImage        m_fxaaImage;
    VkDeviceMemory m_fxaaMemory;
    VkImageView    m_fxaaImageView;
    VkFramebuffer  m_fxaaFramebuffer;

    // 管线（裸 VkPipeline，与 sky/grid 一致）
    VkPipeline       m_bloomDownsamplePipeline;
    VkPipelineLayout m_bloomDownsampleLayout;
    VkPipeline       m_bloomUpsamplePipeline;
    VkPipelineLayout m_bloomUpsampleLayout;
    VkPipeline       m_compositePipeline;
    VkPipelineLayout m_compositeLayout;
    VkPipeline       m_fxaaPipeline;
    VkPipelineLayout m_fxaaLayout;

    // RenderPass
    VkRenderPass   m_bloomDownsampleRenderPass;  // HDR, LoadOp=DONT_CARE
    VkRenderPass   m_bloomUpsampleRenderPass;    // HDR, LoadOp=LOAD
    VkRenderPass   m_ldrRenderPass;              // LDR UNORM, LoadOp=DONT_CARE

    // 共享资源
    VkSampler      m_linearSampler;              // 线性过滤 + clamp to edge
    VkDescriptorSetLayout m_postProcessSetLayout;
    VkDescriptorPool      m_descriptorPool;      // 后处理专用 pool

    VulkanContext* m_context = nullptr;
    uint32_t       m_width = 0, m_height = 0;
};
```

### 与 Renderer 集成

```cpp
// Renderer.h 新增成员
PostProcessPipeline m_postProcess;

// Renderer::init()
m_postProcess.init(m_context, m_layoutCache, width, height);

// drawScene() 之后、blitToSwapchain() 之前
auto& ppSettings = scene.getPostProcessSettings();
m_postProcess.execute(cmd, m_offscreenImageView, ppSettings);

// blitToSwapchain() 修改 blit 源
// 原来：blit m_offscreenImage → swapchain
// 改为：blit m_postProcess.getFinalImage(ppSettings) → swapchain

// resizeOffscreen() 中
m_postProcess.resize(newWidth, newHeight);

// reloadShaders() 中
m_postProcess.reloadShaders();
```

### 对现有代码的修改范围

- `Renderer.h/cpp`：
  - 加 `PostProcessPipeline` 成员
  - offscreen 格式改为 `R16G16B16A16_SFLOAT`
  - offscreen RenderPass 重建（format 变更）
  - 所有使用 offscreen RenderPass 的 pipeline 重建（sky/grid/scene/wireframe/bindless）
  - `blitToSwapchain()` 修改 blit 源
  - `reloadShaders()` 增加后处理管线重载
  - `createFullscreenBundlePipeline()` 扩展支持 push constant 和 blend state
  - 新增 `getDisplayImageView()` 接口：返回后处理最终输出的 image view（供编辑器视口显示）
- `Scene.h/cpp` — 增加 `PostProcessSettings` 成员和序列化/反序列化
- `EditorApp.cpp` — 注册 PostProcessPanel 面板，传递 Scene 引用
- `editor/panels/SceneViewPanel.cpp` — `recreateDescriptorSet()` 改为调用 `renderer.getDisplayImageView()` 替代 `renderer.getOffscreenImageView()`，确保编辑器视口显示后处理结果而非原始 HDR 图像

### Inspector 面板

新增 `PostProcessPanel`，用 ImGui 控件暴露所有参数：
- Checkbox：各效果开关
- DragFloat / SliderFloat：数值参数（带范围限制）
- 四个折叠分组：Bloom / Tone Mapping / Color Grading / FXAA
