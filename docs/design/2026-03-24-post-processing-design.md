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

## 资源（Render Target）规划

| RT 名称 | 格式 | 分辨率 | 用途 |
|---------|------|--------|------|
| m_offscreenImage | R16G16B16A16_SFLOAT | 全分辨率 | 场景 HDR 渲染结果（已有，改格式） |
| m_offscreenDepthImage | D32_SFLOAT | 全分辨率 | 深度（已有，不变） |
| m_bloomMipChain[] | R16G16B16A16_SFLOAT | 1/2 → 1/2^N | Bloom 降采样/升采样 mip 链 |
| m_compositeImage | R8G8B8A8_SRGB | 全分辨率 | Composite 输出（LDR） |
| m_fxaaImage | R8G8B8A8_SRGB | 全分辨率 | FXAA 输出 → 最终 blit 源 |

### Bloom Mip Chain

- 使用单张纹理的多个 mip level，减少资源管理复杂度
- mip 0 = 1/2 分辨率（bright extract 输出），mip 1 = 1/4，... 最多 5-6 级
- Downsample 阶段：mip N → mip N+1（逐级缩小 + 模糊）
- Upsample 阶段：mip N+1 → mip N（逐级放大 + 与上一级叠加）
- 最终 Bloom 结果在 mip 0

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

### Bloom Pass Vulkan 实现

- 每个 mip level 创建单独的 VkImageView（指定 baseMipLevel）
- 每个 pass 一个轻量 VkRenderPass + VkFramebuffer（单色彩附件，无深度）
- 用全屏三角形绘制（与现有 sky/grid pass 一致）
- pass 之间通过 pipeline barrier 同步（SHADER_READ ↔ COLOR_ATTACHMENT）

### Bloom Push Constant

```cpp
struct BloomPushConstant {
    vec2 texelSize;      // 当前 mip 的 1/width, 1/height
    float threshold;     // 亮度阈值（仅 bright extract 使用）
    float intensity;     // Bloom 强度
    int mipLevel;        // 当前处理的 mip 级别
    int useBrightPass;   // 是否启用亮度提取（首次 pass = 1）
};
```

## Composite Pass

单个全屏 pass 完成 Bloom 合成 + Tone Mapping + Color Grading。

- 输入：m_offscreenImage (HDR 场景) + m_bloomMipChain mip 0 (Bloom 结果)
- 输出：m_compositeImage (LDR R8G8B8A8_SRGB)

着色器内执行顺序：
1. 采样 HDR 场景色
2. 采样 Bloom 纹理，按 bloomIntensity 加权叠加
3. 乘以 exposure
4. ACES Filmic Tone Mapping（HDR → LDR）
5. Color Grading：adjustContrast → adjustSaturation → adjustTemperature → brightness
6. clamp(0, 1) 输出

## FXAA Pass

独立全屏 pass，必须在 LDR 输出之后执行（需要采样邻居像素的 LDR 亮度）。

- 输入：m_compositeImage (LDR)
- 输出：m_fxaaImage (LDR)
- 算法：FXAA 3.11 Quality
  1. 计算当前像素及上下左右邻居的亮度 (dot(rgb, luma_weights))
  2. 对比度检测 → 跳过低对比度区域
  3. 沿梯度方向搜索边缘端点
  4. 根据边缘位置做子像素偏移采样

### FXAA Push Constant

```cpp
struct FxaaPushConstant {
    vec2 texelSize;         // 1/width, 1/height
    float subpixQuality;    // 子像素质量（默认 0.75）
    float edgeThreshold;    // 边缘阈值（默认 0.166）
    float edgeThresholdMin; // 最小阈值（默认 0.0833）
};
```

## PostProcess 参数

```cpp
struct PostProcessSettings {
    // Bloom
    bool  bloomEnabled = true;
    float bloomThreshold = 1.0f;     // HDR 亮度提取阈值
    float bloomIntensity = 0.5f;     // Bloom 混合强度
    int   bloomMipCount = 5;         // Bloom mip 级数

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

存放在 Renderer 中作为成员变量，通过 `getPostProcessSettings()` 返回引用供 Inspector 面板读写。随场景 JSON 序列化（增加 `"postProcess"` 节点）。

## 着色器文件清单

```
assets/shaders/postprocess/
├── bloom_downsample.slang      # Bright Extract + 降采样（13-tap filter）
├── bloom_upsample.slang        # 升采样 + 叠加（tent filter）
├── composite.slang             # Bloom合成 + ToneMapping + ColorGrading
├── fxaa.slang                  # FXAA 3.11 Quality
└── fullscreen.slang            # 共享的全屏三角形顶点着色器
```

### 描述符集设计

所有后处理 pass 使用同一个 descriptor set layout：
- set 0 binding 0: sampler2D（主输入纹理）
- set 0 binding 1: sampler2D（可选，Composite pass 用于 Bloom 纹理）

参数全部通过 push constant 传递，不需要 UBO。

## 代码结构

### 新增文件

```
engine/renderer/PostProcess.h/cpp     # PostProcessSettings + PostProcessPipeline 类
editor/panels/PostProcessPanel.h/cpp  # Inspector 面板
assets/shaders/postprocess/*.slang    # 后处理着色器
```

### PostProcessPipeline 类

```cpp
class PostProcessPipeline {
public:
    void init(VulkanContext& ctx, DescriptorLayoutCache& layoutCache,
              uint32_t width, uint32_t height);
    void destroy();
    void resize(uint32_t width, uint32_t height);

    void execute(VkCommandBuffer cmd,
                 VkImageView sceneHDR,
                 const PostProcessSettings& settings);

    VkImage     getFinalImage() const;
    VkImageView getFinalImageView() const;

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
    VkImage        m_bloomMipImage;
    VkDeviceMemory m_bloomMipMemory;
    VkImageView    m_bloomMipViews[6];
    VkFramebuffer  m_bloomFramebuffers[6];

    // Composite 资源
    VkImage        m_compositeImage;
    VkImageView    m_compositeImageView;
    VkFramebuffer  m_compositeFramebuffer;

    // FXAA 资源
    VkImage        m_fxaaImage;
    VkImageView    m_fxaaImageView;
    VkFramebuffer  m_fxaaFramebuffer;

    // 管线
    Pipeline       m_bloomDownsamplePipeline;
    Pipeline       m_bloomUpsamplePipeline;
    Pipeline       m_compositePipeline;
    Pipeline       m_fxaaPipeline;

    // RenderPass
    VkRenderPass   m_postProcessRenderPass;  // 输出 HDR — bloom 用
    VkRenderPass   m_ldrRenderPass;          // 输出 LDR — composite/fxaa 用

    // 共享资源
    VkSampler      m_linearSampler;
    VkDescriptorSetLayout m_postProcessSetLayout;

    VulkanContext* m_context = nullptr;
    uint32_t       m_width = 0, m_height = 0;
};
```

### 与 Renderer 集成

```cpp
// Renderer.h 新增成员
PostProcessPipeline m_postProcess;
PostProcessSettings m_postProcessSettings;

// Renderer::init()
m_postProcess.init(m_context, m_layoutCache, width, height);

// drawScene() 之后、blitToSwapchain() 之前
m_postProcess.execute(cmd, m_offscreenImageView, m_postProcessSettings);

// blitToSwapchain() 修改 blit 源
// 原来：blit m_offscreenImage → swapchain
// 改为：blit m_postProcess.getFinalImage() → swapchain

// resizeOffscreen() 中
m_postProcess.resize(newWidth, newHeight);
```

### 对现有代码的修改范围

- `Renderer.h/cpp` — 加成员、改 offscreen 格式为 HDR、改 blit 源、调用 PostProcessPipeline
- `Scene` 序列化 — 增加 `postProcess` 字段读写
- `EditorApp.cpp` — 无变化（渲染流程不变）

### Inspector 面板

新增 `PostProcessPanel`，用 ImGui 控件暴露所有参数：
- Checkbox：各效果开关
- DragFloat / SliderFloat：数值参数（带范围限制）
- 四个折叠分组：Bloom / Tone Mapping / Color Grading / FXAA
