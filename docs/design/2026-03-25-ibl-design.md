# IBL（基于图像的光照）设计

## 概述

为 QymEngine 的 PBR 渲染管线添加 Image-Based Lighting（IBL），使用 Split-Sum 近似实现漫反射和镜面反射环境光照。运行时从现有全景 HDR 天空盒纹理生成所有预计算贴图。

## 需求

- 漫反射 IBL：Irradiance cubemap（替代当前 ambientColor 常量）
- 镜面反射 IBL：Pre-filtered environment cubemap + BRDF LUT
- 运行时 GPU 生成（场景加载 / 天空盒更换时触发）
- 用 render pass 做卷积（不引入 compute shader）
- 集成到现有 Lit.slang PBR 着色器

## 整体架构

```
全景 HDR 纹理 (equirectangular)
    ↓ [Equirect-to-Cubemap pass]
HDR Cubemap (6面, 512×512)
    ├── [Irradiance Convolution pass] → Irradiance Cubemap (6面, 32×32)
    ├── [Pre-filter Convolution pass] → Pre-filtered Cubemap (6面, 128×128, 5 mip levels)
    └── [BRDF Integration pass] → BRDF LUT (512×512, R16G16_SFLOAT)

场景渲染时:
  Lit.slang 采样 irradiance cubemap (漫反射) + pre-filtered cubemap + BRDF LUT (镜面反射)
```

## 资源规划

| 资源 | 格式 | 尺寸 | 用途 |
|------|------|------|------|
| HDR Cubemap | R16G16B16A16_SFLOAT | 512×512×6 | 中间产物，从全景图转换 |
| Irradiance Cubemap | R16G16B16A16_SFLOAT | 32×32×6 | 漫反射环境光 |
| Pre-filtered Cubemap | R16G16B16A16_SFLOAT | 128×128×6, 5 mip | 镜面反射（按粗糙度分 mip） |
| BRDF LUT | R16G16_SFLOAT | 512×512 | Split-Sum 第二项查找表 |

BRDF LUT 只需生成一次（与场景/天空盒无关），可在 init 时生成。

## 生成管线（4 个 pass）

### Pass 1: Equirectangular → Cubemap

将全景 HDR 纹理转换为 cubemap 的 6 个面。

- 渲染 6 次（每面一次），用不同的 view 矩阵
- 着色器：全屏三角形，采样全景图，输出到 cubemap face
- RenderPass: HDR format, 512×512, LoadOp=DONT_CARE

### Pass 2: Irradiance Convolution

对 cubemap 做半球卷积（cosine-weighted），生成漫反射 irradiance map。

- 渲染 6 次（每面一次）
- 着色器：对每个输出像素，在其法线方向的半球上采样 cubemap 并加权平均
- 采样数：~64-128 个方向（性能 vs 质量平衡）
- RenderPass: HDR format, 32×32, LoadOp=DONT_CARE

### Pass 3: Pre-filtered Environment Map

按粗糙度等级做 GGX importance sampling 卷积。

- 渲染 6面 × 5 mip = 30 次
- 每个 mip 对应一个粗糙度：mip 0 = roughness 0（镜面反射），mip 4 = roughness 1（漫反射）
- 着色器：GGX importance sampling，采样数随 mip 增加（mip0: 256 samples, mip4: 32 samples）
- RenderPass: HDR format, 128→64→32→16→8, LoadOp=DONT_CARE

### Pass 4: BRDF Integration LUT

预计算 BRDF 的 Split-Sum 第二项：F0 * scale + bias。

- 只渲染一次（2D 全屏 pass）
- 输入：UV.x = NdotV, UV.y = roughness
- 输出：R = scale, G = bias
- 着色器：GGX importance sampling 积分
- RenderPass: R16G16_SFLOAT, 512×512, LoadOp=DONT_CARE
- 此 pass 与场景无关，引擎 init 时生成一次

## 着色器清单

```
assets/shaders/ibl/
├── EquirectToCubemap.slang    # 全景图 → cubemap 转换
├── IrradianceConvolution.slang # 漫反射卷积
├── PrefilterEnvMap.slang       # 镜面反射 GGX 卷积
└── BrdfLut.slang               # BRDF LUT 生成
```

所有着色器使用全屏三角形顶点着色器。片段着色器通过 push constant 接收当前渲染的 cubemap face 索引和 roughness 值。

### Cubemap 采样方向计算

每个 face 的 fragment shader 需要从 UV 计算采样方向。push constant 传入 face index（0-5），着色器根据 face 和 UV 计算世界空间方向：

```hlsl
float3 uvToDirection(float2 uv, int face) {
    float2 st = uv * 2.0 - 1.0;
    switch (face) {
        case 0: return normalize(float3( 1, -st.y, -st.x));  // +X
        case 1: return normalize(float3(-1, -st.y,  st.x));  // -X
        case 2: return normalize(float3( st.x,  1,  st.y));  // +Y
        case 3: return normalize(float3( st.x, -1, -st.y));  // -Y
        case 4: return normalize(float3( st.x, -st.y,  1));  // +Z
        case 5: return normalize(float3(-st.x, -st.y, -1));  // -Z
    }
}
```

## Lit.slang 集成

当前环境光计算（Lit.slang 中）：
```hlsl
float3 ambient = frame.ambientColor * albedo;
```

替换为 IBL：
```hlsl
// 漫反射 IBL
float3 N = normal;
float3 irradiance = irradianceMap.Sample(N).rgb;
float3 diffuseIBL = irradiance * albedo * (1.0 - metallic);

// 镜面反射 IBL
float3 R = reflect(-V, N);
float3 prefilteredColor = prefilteredMap.SampleLevel(R, roughness * 4.0).rgb;
float2 brdf = brdfLUT.Sample(float2(NdotV, roughness)).rg;
float3 F0 = lerp(float3(0.04), albedo, metallic);
float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

float3 ambient = diffuseIBL + specularIBL;
```

### 描述符集变更

当前 Set 0 (PerFrame)：
- binding 0: FrameData UBO
- binding 1: Shadow map sampler

需要增加 IBL 纹理绑定：
- binding 2: Irradiance cubemap sampler
- binding 3: Pre-filtered cubemap sampler
- binding 4: BRDF LUT sampler

这会影响 `m_perFrameLayout` 和所有使用它的 pipeline。

## 代码结构

### 新增文件

```
engine/renderer/IBLGenerator.h/cpp    # IBL 贴图运行时生成
assets/shaders/ibl/*.slang            # 4 个 IBL 生成着色器
```

### IBLGenerator 类

```cpp
class IBLGenerator {
public:
    void init(VulkanContext& ctx, DescriptorLayoutCache& layoutCache);
    void destroy();

    // 从全景 HDR 纹理生成所有 IBL 贴图
    void generate(VkImageView panoramaView, VkSampler panoramaSampler);

    // 生成 BRDF LUT（只需调用一次）
    void generateBrdfLut();

    VkImageView getIrradianceView() const;
    VkImageView getPrefilteredView() const;
    VkImageView getBrdfLutView() const;
    VkSampler   getCubemapSampler() const;

private:
    void createResources();
    void destroyResources();
    void createPipelines();
    void destroyPipelines();

    void generateCubemap(VkImageView panoramaView, VkSampler panoramaSampler);
    void generateIrradiance();
    void generatePrefiltered();

    // Cubemap 资源
    VkImage m_cubemapImage, m_irradianceImage, m_prefilteredImage, m_brdfLutImage;
    // ... memory, views, samplers, pipelines, render passes, framebuffers
};
```

### 与 Renderer 集成

1. `Renderer` 持有 `IBLGenerator` 成员
2. `Renderer::init()` 中调用 `m_iblGenerator.init()` 和 `generateBrdfLut()`
3. 天空盒全景图加载后调用 `m_iblGenerator.generate(skyPanoramaView, ...)`
4. `updateUniformBuffer()` 中将 IBL 贴图绑定到 set 0 的 binding 2/3/4
5. `Lit.slang` 中采样这些贴图做 IBL 光照

### 触发时机

- 引擎启动：加载默认场景的天空盒 → 生成 IBL
- 天空盒更换：重新生成 IBL（销毁旧贴图 → 重新生成）
- BRDF LUT：init 时生成一次，永不更新

## 对现有代码的修改

- `Renderer.h/cpp` — 加 IBLGenerator 成员、扩展 perFrameLayout（5 个 binding）、更新 descriptor set 写入
- `Lit.slang` — 环境光计算替换为 IBL 采样
- `Buffer.h` — FrameData UBO 可能需要加 `iblEnabled` 标志（或直接在 IBL 未生成时绑定黑色 fallback）
- `Sky.slang` — 不变（天空盒渲染不受影响）
- 所有使用 perFrameLayout 的 pipeline 需要重建（因为 layout 增加了 binding）

## 性能考虑

- IBL 生成是一次性开销（场景加载时），不影响帧渲染
- Irradiance convolution: 32×32×6 面，每面 ~100 采样 → ~20K 采样/面，极快
- Pre-filter: 128×128 × 6面 × 5 mip，最多 256 采样/像素 → 最慢的 pass，但只执行一次
- BRDF LUT: 512×512 × 1024 采样 → 较慢但只执行一次
- 运行时采样：每个场景像素多 3 次纹理采样（irradiance + prefiltered + brdfLUT），开销可忽略
