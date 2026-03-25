# 后处理系统实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 QymEngine 实现完整的后处理管线（Bloom + Tone Mapping + Color Grading + FXAA）

**Architecture:** 将 offscreen 升级为 HDR (R16G16B16A16_SFLOAT)，在场景渲染和 blit 之间插入后处理链。Bloom 为多 pass（降采样+升采样），Composite pass 合并 Tone Mapping + Color Grading，FXAA 为独立 pass。所有后处理管线封装在 PostProcessPipeline 类中。

**Tech Stack:** C++17, Vulkan 1.4, Slang shaders, ImGui (Inspector)

**Spec:** `docs/design/2026-03-24-post-processing-design.md`

---

## 文件结构

### 新建文件
| 文件 | 职责 |
|------|------|
| `engine/renderer/PostProcess.h` | PostProcessSettings 结构体 + PostProcessPipeline 类声明 |
| `engine/renderer/PostProcess.cpp` | PostProcessPipeline 完整实现（资源创建/销毁、execute、resize） |
| `editor/panels/PostProcessPanel.h` | PostProcessPanel 类声明 |
| `editor/panels/PostProcessPanel.cpp` | Inspector 面板实现（ImGui 控件） |
| `assets/shaders/postprocess/Composite.slang` | Bloom 合成 + Tone Mapping + Color Grading |
| `assets/shaders/postprocess/FXAA.slang` | FXAA 3.11 |
| `assets/shaders/postprocess/BloomDownsample.slang` | Bright Extract + 13-tap 降采样 |
| `assets/shaders/postprocess/BloomUpsample.slang` | Tent filter 升采样 |

### 修改文件
| 文件 | 修改内容 |
|------|---------|
| `CMakeLists.txt` | 扩展 shader GLOB 模式包含 postprocess/ 子目录 |
| `engine/renderer/Renderer.h` | 加 PostProcessPipeline 成员、getDisplayImageView 接口、m_displayImage 缓存 |
| `engine/renderer/Renderer.cpp:49-189` | 扩展 createFullscreenBundlePipeline 支持 push constant + blend state |
| `engine/renderer/Renderer.cpp:1030-1047` | offscreen format 改为 R16G16B16A16_SFLOAT |
| `engine/renderer/Renderer.cpp:1159-1163` | offscreen RenderPass format 同步 |
| `engine/renderer/Renderer.cpp:340-401` | drawScene 中插入 postProcess.execute 调用 |
| `engine/renderer/Renderer.cpp:403-473` | blitToSwapchain 改为 blit 后处理输出（含完整 barrier 修改） |
| `engine/renderer/Renderer.cpp:1502-1607` | reloadShaders 增加后处理管线重载 |
| `engine/scene/Scene.h` | 加 PostProcessSettings 成员和 getter |
| `engine/scene/Scene.cpp:176-207,238-261` | serialize/deserialize/toJsonString/fromJsonString 增加 postProcess 节点 |
| `editor/EditorApp.h` | 加 PostProcessPanel 成员 |
| `editor/EditorApp.cpp:403-408` | 注册 PostProcessPanel 渲染 |
| `editor/panels/SceneViewPanel.h` | recreateDescriptorSet/applyPendingResize 签名增加 Scene 参数 |
| `editor/panels/SceneViewPanel.cpp:295-314` | 改为获取后处理输出的 image view |

---

## Task 1: PostProcessSettings 数据结构 + Scene 序列化

**Files:**
- Modify: `engine/renderer/PostProcess.h` (新建)
- Modify: `engine/scene/Scene.h:10-56`
- Modify: `engine/scene/Scene.cpp:176-261`

- [ ] **Step 1: 创建 PostProcess.h 头文件（仅 Settings 部分）**

在 `engine/renderer/PostProcess.h` 中定义 PostProcessSettings：

```cpp
#pragma once
#include <algorithm>

namespace QymEngine {

static constexpr int MAX_BLOOM_MIPS = 6;

struct PostProcessSettings {
    // Bloom
    bool  bloomEnabled = true;
    float bloomThreshold = 1.0f;
    float bloomIntensity = 0.5f;
    int   bloomMipCount = 5;

    // Tone Mapping
    bool  toneMappingEnabled = true;
    float exposure = 1.0f;

    // Color Grading
    bool  colorGradingEnabled = true;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float temperature = 0.0f;
    float tint = 0.0f;
    float brightness = 0.0f;

    // FXAA
    bool  fxaaEnabled = true;
    float fxaaSubpixQuality = 0.75f;
    float fxaaEdgeThreshold = 0.166f;
    float fxaaEdgeThresholdMin = 0.0833f;

    void clampValues() {
        bloomMipCount = std::clamp(bloomMipCount, 1, MAX_BLOOM_MIPS);
        exposure = std::max(exposure, 0.001f);
        contrast = std::clamp(contrast, 0.5f, 2.0f);
        saturation = std::clamp(saturation, 0.0f, 2.0f);
        temperature = std::clamp(temperature, -1.0f, 1.0f);
        tint = std::clamp(tint, -1.0f, 1.0f);
        brightness = std::clamp(brightness, -1.0f, 1.0f);
    }
};

} // namespace QymEngine
```

- [ ] **Step 2: 在 Scene 中添加 PostProcessSettings**

在 `engine/scene/Scene.h` 中：
- 添加 `#include "renderer/PostProcess.h"`
- 在 Scene 类中添加成员和 getter：
```cpp
PostProcessSettings m_postProcessSettings;
// public:
PostProcessSettings& getPostProcessSettings() { return m_postProcessSettings; }
const PostProcessSettings& getPostProcessSettings() const { return m_postProcessSettings; }
```

- [ ] **Step 3: 扩展 Scene 序列化**

在 `engine/scene/Scene.cpp` 的 `serialize()` 方法中，`j["scene"]["nodes"]` 之后添加 postProcess 序列化：
```cpp
// 后处理参数
auto& pp = m_postProcessSettings;
j["scene"]["postProcess"] = {
    {"bloomEnabled", pp.bloomEnabled},
    {"bloomThreshold", pp.bloomThreshold},
    {"bloomIntensity", pp.bloomIntensity},
    {"bloomMipCount", pp.bloomMipCount},
    {"toneMappingEnabled", pp.toneMappingEnabled},
    {"exposure", pp.exposure},
    {"colorGradingEnabled", pp.colorGradingEnabled},
    {"contrast", pp.contrast},
    {"saturation", pp.saturation},
    {"temperature", pp.temperature},
    {"tint", pp.tint},
    {"brightness", pp.brightness},
    {"fxaaEnabled", pp.fxaaEnabled},
    {"fxaaSubpixQuality", pp.fxaaSubpixQuality},
    {"fxaaEdgeThreshold", pp.fxaaEdgeThreshold},
    {"fxaaEdgeThresholdMin", pp.fxaaEdgeThresholdMin}
};
```

在 `deserialize()` 方法中，nodes 反序列化之后添加：
```cpp
if (sceneJson.contains("postProcess")) {
    auto& ppj = sceneJson["postProcess"];
    auto& pp = m_postProcessSettings;
    pp.bloomEnabled = ppj.value("bloomEnabled", true);
    pp.bloomThreshold = ppj.value("bloomThreshold", 1.0f);
    pp.bloomIntensity = ppj.value("bloomIntensity", 0.5f);
    pp.bloomMipCount = ppj.value("bloomMipCount", 5);
    pp.toneMappingEnabled = ppj.value("toneMappingEnabled", true);
    pp.exposure = ppj.value("exposure", 1.0f);
    pp.colorGradingEnabled = ppj.value("colorGradingEnabled", true);
    pp.contrast = ppj.value("contrast", 1.0f);
    pp.saturation = ppj.value("saturation", 1.0f);
    pp.temperature = ppj.value("temperature", 0.0f);
    pp.tint = ppj.value("tint", 0.0f);
    pp.brightness = ppj.value("brightness", 0.0f);
    pp.fxaaEnabled = ppj.value("fxaaEnabled", true);
    pp.fxaaSubpixQuality = ppj.value("fxaaSubpixQuality", 0.75f);
    pp.fxaaEdgeThreshold = ppj.value("fxaaEdgeThreshold", 0.166f);
    pp.fxaaEdgeThresholdMin = ppj.value("fxaaEdgeThresholdMin", 0.0833f);
    pp.clampValues();
}
```

在 `toJsonString()` 方法中（Scene.cpp:238-245），`j["scene"]["nodes"]` 之后添加相同的 postProcess 序列化代码。

在 `fromJsonString()` 方法中（Scene.cpp:247-261），nodes 反序列化之后添加相同的 postProcess 反序列化代码。

- [ ] **Step 4: 编译验证**

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug
```

预期：编译通过，无功能变化。

- [ ] **Step 5: 提交**

```bash
git add engine/renderer/PostProcess.h engine/scene/Scene.h engine/scene/Scene.cpp
git commit -m "feat(postprocess): add PostProcessSettings and scene serialization"
```

---

## Task 2: 扩展 createFullscreenBundlePipeline

**Files:**
- Modify: `engine/renderer/Renderer.cpp:49-189`

- [ ] **Step 1: 给 createFullscreenBundlePipeline 添加参数**

在 `Renderer.cpp:49` 的函数签名中增加两个参数（放在 `debugName` 之前）：
```cpp
static void createFullscreenBundlePipeline(
    VkDevice device,
    VkRenderPass renderPass,
    const std::vector<VkDescriptorSetLayout>& setLayouts,
    const std::string& bundlePath,
    const std::string& variant,
    bool blendEnable,
    bool depthTestEnable,
    bool depthWriteEnable,
    VkPipeline& outPipeline,
    VkPipelineLayout& outLayout,
    const char* debugName,
    const VkPushConstantRange* pushConstantRange = nullptr,
    const VkPipelineColorBlendAttachmentState* customBlendState = nullptr)
```

- [ ] **Step 2: 使用新参数**

在函数体内 `layoutInfo` 创建处（约第 161-164 行）添加 push constant：
```cpp
layoutInfo.pushConstantRangeCount = pushConstantRange ? 1 : 0;
layoutInfo.pPushConstantRanges = pushConstantRange;
```

在 `colorBlendAttachment` 创建处（约第 127-136 行），如果提供了自定义 blend state，则使用：
```cpp
if (customBlendState) {
    colorBlendAttachment = *customBlendState;
} else {
    colorBlendAttachment.blendEnable = blendEnable ? VK_TRUE : VK_FALSE;
    // ... 保持现有默认 blend 配置
}
```

- [ ] **Step 3: 编译验证**

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug
```

预期：编译通过，现有调用点使用默认参数不受影响。

- [ ] **Step 4: 提交**

```bash
git add engine/renderer/Renderer.cpp
git commit -m "feat(renderer): extend createFullscreenBundlePipeline with push constant and blend state"
```

---

## Task 3: HDR Offscreen 格式升级

**Files:**
- Modify: `engine/renderer/Renderer.cpp:1030-1047` (createOffscreen format)
- Modify: `engine/renderer/Renderer.cpp:1159-1163` (RenderPass format)

- [ ] **Step 1: 将 offscreen 格式改为 HDR**

在 `Renderer::createOffscreen()` 第 1033 行，将：
```cpp
VkFormat format = m_swapChain.getImageFormat();
```
改为：
```cpp
VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
```

- [ ] **Step 2: 强制重建 RenderPass**

当前 offscreen RenderPass 只在 `m_offscreenRenderPass == VK_NULL_HANDLE` 时创建（第 1160 行）。由于 format 变了，需要确保 RenderPass 与新 format 匹配。如果 RenderPass 在 format 改动前已创建（例如之前用 swapchain format），需要销毁并重建。

最简单的做法：`createOffscreen` 中 format 已经以局部变量传入 RenderPass 创建代码，只要确保第一次调用时用新 format 即可。由于引擎启动时先 createOffscreen 再创建 pipeline，这步自然生效。

- [ ] **Step 3: 编译并运行验证**

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug
```

运行 QymEditor.exe，预期：
- 场景仍然正常渲染（HDR format 只是精度更高，渲染内容不变）
- 但颜色可能看起来不同（HDR 值没有经过 tone mapping 直接 blit 到 SRGB swapchain）

这是预期行为，后续 Composite pass 会处理 HDR→LDR 转换。

- [ ] **Step 4: 提交**

```bash
git add engine/renderer/Renderer.cpp
git commit -m "feat(renderer): upgrade offscreen to HDR R16G16B16A16_SFLOAT"
```

---

## Task 4: CMake 着色器路径 + Composite 着色器

**Files:**
- Modify: `CMakeLists.txt:82-84`
- Create: `assets/shaders/postprocess/Composite.slang`

- [ ] **Step 0: 扩展 CMake shader GLOB 包含子目录**

在 `CMakeLists.txt` 第 82-84 行，将：
```cmake
file(GLOB CONFIGURE_DEPENDS SHADER_SLANG_FILES
    "${SHADER_SOURCE_DIR}/*.slang"
)
```
改为：
```cmake
file(GLOB_RECURSE CONFIGURE_DEPENDS SHADER_SLANG_FILES
    "${SHADER_SOURCE_DIR}/*.slang"
)
```

这样 `assets/shaders/postprocess/*.slang` 也会被自动发现并编译。

- [ ] **Step 1: 编写 Composite.slang**

```hlsl
// Composite 后处理: Bloom 混合 + Tone Mapping (ACES) + Color Grading + Gamma 矫正

[[vk::binding(0, 0)]] Sampler2D sceneHDR;
[[vk::binding(1, 0)]] Sampler2D bloomTex;

struct CompositePushConstant {
    float2 texelSize;
    float exposure;
    float bloomIntensity;
    int bloomEnabled;
    int toneMappingEnabled;
    int colorGradingEnabled;
    float contrast;
    float saturation;
    float temperature;
    float tint;
    float brightness;
};

[[vk::push_constant]] CompositePushConstant pc;

struct VSOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

static const float3 positions[6] = {
    float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0),
    float3(-1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)
};

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID) {
    VSOutput o;
    float3 p = positions[vertexID];
    o.sv_position = float4(p, 1.0);
    o.uv = p.xy * 0.5 + 0.5;
    o.uv.y = 1.0 - o.uv.y; // 翻转 Y
    return o;
}

// ACES Filmic Tone Mapping
float3 ACESFilm(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// sRGB gamma 矫正 (linear → sRGB)
float3 linearToSRGB(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return float3(
        c.x <= 0.0031308 ? lo.x : hi.x,
        c.y <= 0.0031308 ? lo.y : hi.y,
        c.z <= 0.0031308 ? lo.z : hi.z
    );
}

float3 adjustContrast(float3 color, float contrastVal) {
    float3 midgray = float3(0.5, 0.5, 0.5);
    return midgray + (color - midgray) * contrastVal;
}

float3 adjustSaturation(float3 color, float saturationVal) {
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    return lerp(float3(luma, luma, luma), color, saturationVal);
}

float3 adjustTemperature(float3 color, float temp, float tintVal) {
    // 简化色温模型: 暖色偏红，冷色偏蓝
    color.r += temp * 0.1;
    color.b -= temp * 0.1;
    // 色调: 正值偏品红，负值偏绿
    color.g -= tintVal * 0.1;
    return color;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float3 color = sceneHDR.Sample(input.uv).rgb;

    // Bloom 合成
    if (pc.bloomEnabled != 0) {
        float3 bloom = bloomTex.Sample(input.uv).rgb;
        color += bloom * pc.bloomIntensity;
    }

    // 曝光
    color *= pc.exposure;

    // Tone Mapping
    if (pc.toneMappingEnabled != 0) {
        color = ACESFilm(color);
    } else {
        color = saturate(color);
    }

    // Color Grading
    if (pc.colorGradingEnabled != 0) {
        color = adjustContrast(color, pc.contrast);
        color = adjustSaturation(color, pc.saturation);
        color = adjustTemperature(color, pc.temperature, pc.tint);
        color += pc.brightness;
    }

    // 手动 gamma 矫正 (输出到 UNORM)
    color = linearToSRGB(saturate(color));

    return float4(color, 1.0);
}
```

- [ ] **Step 2: 编译着色器**

```bash
cd E:/MYQ/QymEngine
build3/tools/shader_compiler/Debug/ShaderCompiler.exe assets/shaders assets/shaders
```

预期：生成 `assets/shaders/postprocess/Composite.shaderbundle`

- [ ] **Step 3: 提交**

```bash
git add assets/shaders/postprocess/Composite.slang
git commit -m "feat(shaders): add composite post-process shader"
```

---

## Task 5: FXAA 着色器

**Files:**
- Create: `assets/shaders/postprocess/FXAA.slang`

- [ ] **Step 1: 编写 FXAA.slang**

```hlsl
// FXAA 3.11 Quality — 在 gamma 空间工作

[[vk::binding(0, 0)]] Sampler2D inputTex;

struct FxaaPushConstant {
    float2 texelSize;
    float subpixQuality;
    float edgeThreshold;
    float edgeThresholdMin;
};

[[vk::push_constant]] FxaaPushConstant pc;

struct VSOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

static const float3 positions[6] = {
    float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0),
    float3(-1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)
};

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID) {
    VSOutput o;
    float3 p = positions[vertexID];
    o.sv_position = float4(p, 1.0);
    o.uv = p.xy * 0.5 + 0.5;
    o.uv.y = 1.0 - o.uv.y;
    return o;
}

float luminance(float3 c) {
    return dot(c, float3(0.299, 0.587, 0.114));
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 invSize = pc.texelSize;

    // 采样中心及四邻域
    float3 rgbM  = inputTex.Sample(uv).rgb;
    float3 rgbN  = inputTex.Sample(uv + float2(0, -invSize.y)).rgb;
    float3 rgbS  = inputTex.Sample(uv + float2(0,  invSize.y)).rgb;
    float3 rgbW  = inputTex.Sample(uv + float2(-invSize.x, 0)).rgb;
    float3 rgbE  = inputTex.Sample(uv + float2( invSize.x, 0)).rgb;

    float lumaM = luminance(rgbM);
    float lumaN = luminance(rgbN);
    float lumaS = luminance(rgbS);
    float lumaW = luminance(rgbW);
    float lumaE = luminance(rgbE);

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    float lumaRange = lumaMax - lumaMin;

    // 低对比度区域直接返回
    if (lumaRange < max(pc.edgeThresholdMin, lumaMax * pc.edgeThreshold)) {
        return float4(rgbM, 1.0);
    }

    // 采样对角邻域
    float3 rgbNW = inputTex.Sample(uv + float2(-invSize.x, -invSize.y)).rgb;
    float3 rgbNE = inputTex.Sample(uv + float2( invSize.x, -invSize.y)).rgb;
    float3 rgbSW = inputTex.Sample(uv + float2(-invSize.x,  invSize.y)).rgb;
    float3 rgbSE = inputTex.Sample(uv + float2( invSize.x,  invSize.y)).rgb;

    float lumaNW = luminance(rgbNW);
    float lumaNE = luminance(rgbNE);
    float lumaSW = luminance(rgbSW);
    float lumaSE = luminance(rgbSE);

    // 子像素 aliasing 检测
    float lumaAvg = (lumaN + lumaS + lumaW + lumaE) * 0.25;
    float subpixRcpRange = 1.0 / lumaRange;
    float subpixA = saturate(abs(lumaAvg - lumaM) * subpixRcpRange);
    float subpixB = (-2.0 * subpixA + 3.0) * subpixA * subpixA;
    float subpixC = subpixB * subpixB * pc.subpixQuality;

    // 边缘方向检测
    float edgeH = abs(-2.0 * lumaW + lumaNW + lumaSW) +
                  abs(-2.0 * lumaM + lumaN + lumaS) * 2.0 +
                  abs(-2.0 * lumaE + lumaNE + lumaSE);
    float edgeV = abs(-2.0 * lumaN + lumaNW + lumaNE) +
                  abs(-2.0 * lumaM + lumaW + lumaE) * 2.0 +
                  abs(-2.0 * lumaS + lumaSW + lumaSE);
    bool isHorizontal = (edgeH >= edgeV);

    // 沿边缘法线方向的梯度
    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float gradient1 = abs(luma1 - lumaM);
    float gradient2 = abs(luma2 - lumaM);
    bool is1Steepest = gradient1 >= gradient2;

    float stepLength = isHorizontal ? invSize.y : invSize.x;
    float gradientScaled = 0.25 * max(gradient1, gradient2);
    float lumaLocalAvg;

    if (is1Steepest) {
        stepLength = -stepLength;
        lumaLocalAvg = 0.5 * (luma1 + lumaM);
    } else {
        lumaLocalAvg = 0.5 * (luma2 + lumaM);
    }

    // 沿边缘搜索端点
    float2 currentUv = uv;
    if (isHorizontal) {
        currentUv.y += stepLength * 0.5;
    } else {
        currentUv.x += stepLength * 0.5;
    }

    float2 offset = isHorizontal ? float2(invSize.x, 0) : float2(0, invSize.y);

    float2 uv1 = currentUv - offset;
    float2 uv2 = currentUv + offset;

    float lumaEnd1 = luminance(inputTex.Sample(uv1).rgb) - lumaLocalAvg;
    float lumaEnd2 = luminance(inputTex.Sample(uv2).rgb) - lumaLocalAvg;

    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;

    // 逐步搜索（最多 12 步）
    static const float QUALITY[12] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
    for (int i = 0; i < 12 && !reachedBoth; i++) {
        if (!reached1) {
            uv1 -= offset * QUALITY[i];
            lumaEnd1 = luminance(inputTex.Sample(uv1).rgb) - lumaLocalAvg;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2) {
            uv2 += offset * QUALITY[i];
            lumaEnd2 = luminance(inputTex.Sample(uv2).rgb) - lumaLocalAvg;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
        reachedBoth = reached1 && reached2;
    }

    // 计算到最近端点的距离
    float dist1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float dist2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    float distFinal = min(dist1, dist2);
    float edgeLength = dist1 + dist2;
    float pixelOffset = -distFinal / edgeLength + 0.5;

    // 确定方向是否正确
    bool isLumaMSmaller = lumaM < lumaLocalAvg;
    bool correctVariation = ((dist1 < dist2 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaMSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // 子像素抗锯齿
    finalOffset = max(finalOffset, subpixC);

    // 最终采样
    float2 finalUv = uv;
    if (isHorizontal) {
        finalUv.y += finalOffset * stepLength;
    } else {
        finalUv.x += finalOffset * stepLength;
    }

    float3 finalColor = inputTex.Sample(finalUv).rgb;
    return float4(finalColor, 1.0);
}
```

- [ ] **Step 2: 编译着色器**

```bash
build3/tools/shader_compiler/Debug/ShaderCompiler.exe assets/shaders assets/shaders
```

预期：生成 `assets/shaders/postprocess/FXAA.shaderbundle`

- [ ] **Step 3: 提交**

```bash
git add assets/shaders/postprocess/FXAA.slang
git commit -m "feat(shaders): add FXAA 3.11 post-process shader"
```

---

## Task 6: Bloom 着色器

**Files:**
- Create: `assets/shaders/postprocess/BloomDownsample.slang`
- Create: `assets/shaders/postprocess/BloomUpsample.slang`

- [ ] **Step 1: 编写 BloomDownsample.slang**

```hlsl
// 13-tap downsample filter (CoD 2014) + 可选 bright extract

[[vk::binding(0, 0)]] Sampler2D inputTex;

struct BloomPushConstant {
    float2 texelSize;
    float threshold;
    float intensity;
    int mipLevel;
    int useBrightPass;
};

[[vk::push_constant]] BloomPushConstant pc;

struct VSOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

static const float3 positions[6] = {
    float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0),
    float3(-1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)
};

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID) {
    VSOutput o;
    float3 p = positions[vertexID];
    o.sv_position = float4(p, 1.0);
    o.uv = p.xy * 0.5 + 0.5;
    o.uv.y = 1.0 - o.uv.y;
    return o;
}

// 软阈值裁剪 (soft knee)
float3 brightPass(float3 color) {
    float br = max(color.r, max(color.g, color.b));
    float soft = br - pc.threshold + 0.5; // knee = 0.5
    soft = clamp(soft, 0.0, 1.0);
    soft = soft * soft * (1.0 / 1.0);
    float contribution = max(soft, br - pc.threshold) / max(br, 0.00001);
    return color * max(contribution, 0.0);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 ts = pc.texelSize;

    // 13-tap downsample (4 个 bilinear + 1 个中心)
    // 对应 CoD 2014 presentation 中的权重分布
    float3 a = inputTex.Sample(uv + float2(-2*ts.x, 2*ts.y)).rgb;
    float3 b = inputTex.Sample(uv + float2(0, 2*ts.y)).rgb;
    float3 c = inputTex.Sample(uv + float2(2*ts.x, 2*ts.y)).rgb;

    float3 d = inputTex.Sample(uv + float2(-2*ts.x, 0)).rgb;
    float3 e = inputTex.Sample(uv).rgb;
    float3 f = inputTex.Sample(uv + float2(2*ts.x, 0)).rgb;

    float3 g = inputTex.Sample(uv + float2(-2*ts.x, -2*ts.y)).rgb;
    float3 h = inputTex.Sample(uv + float2(0, -2*ts.y)).rgb;
    float3 i = inputTex.Sample(uv + float2(2*ts.x, -2*ts.y)).rgb;

    float3 j = inputTex.Sample(uv + float2(-ts.x, ts.y)).rgb;
    float3 k = inputTex.Sample(uv + float2(ts.x, ts.y)).rgb;
    float3 l = inputTex.Sample(uv + float2(-ts.x, -ts.y)).rgb;
    float3 m = inputTex.Sample(uv + float2(ts.x, -ts.y)).rgb;

    // 权重组合 (避免 firefly)
    float3 color = e * 0.125;
    color += (a + c + g + i) * 0.03125;
    color += (b + d + f + h) * 0.0625;
    color += (j + k + l + m) * 0.125;

    // Bright extract (仅首次 pass)
    if (pc.useBrightPass != 0) {
        color = brightPass(color);
    }

    return float4(color, 1.0);
}
```

- [ ] **Step 2: 编写 BloomUpsample.slang**

```hlsl
// Tent filter 升采样

[[vk::binding(0, 0)]] Sampler2D inputTex;

struct BloomPushConstant {
    float2 texelSize;
    float threshold;
    float intensity;
    int mipLevel;
    int useBrightPass;
};

[[vk::push_constant]] BloomPushConstant pc;

struct VSOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

static const float3 positions[6] = {
    float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0),
    float3(-1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)
};

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID) {
    VSOutput o;
    float3 p = positions[vertexID];
    o.sv_position = float4(p, 1.0);
    o.uv = p.xy * 0.5 + 0.5;
    o.uv.y = 1.0 - o.uv.y;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 ts = pc.texelSize;

    // 3×3 tent filter
    float3 color = float3(0, 0, 0);
    color += inputTex.Sample(uv + float2(-ts.x, ts.y)).rgb;
    color += inputTex.Sample(uv + float2(0, ts.y)).rgb * 2.0;
    color += inputTex.Sample(uv + float2(ts.x, ts.y)).rgb;

    color += inputTex.Sample(uv + float2(-ts.x, 0)).rgb * 2.0;
    color += inputTex.Sample(uv).rgb * 4.0;
    color += inputTex.Sample(uv + float2(ts.x, 0)).rgb * 2.0;

    color += inputTex.Sample(uv + float2(-ts.x, -ts.y)).rgb;
    color += inputTex.Sample(uv + float2(0, -ts.y)).rgb * 2.0;
    color += inputTex.Sample(uv + float2(ts.x, -ts.y)).rgb;

    color /= 16.0;
    color *= pc.intensity;

    return float4(color, 1.0);
}
```

- [ ] **Step 3: 编译着色器**

```bash
build3/tools/shader_compiler/Debug/ShaderCompiler.exe assets/shaders assets/shaders
```

预期：生成 `BloomDownsample.shaderbundle` 和 `BloomUpsample.shaderbundle`

- [ ] **Step 4: 提交**

```bash
git add assets/shaders/postprocess/BloomDownsample.slang assets/shaders/postprocess/BloomUpsample.slang
git commit -m "feat(shaders): add bloom downsample and upsample shaders"
```

---

## Task 7: PostProcessPipeline 核心实现

**Files:**
- Modify: `engine/renderer/PostProcess.h`（追加 Pipeline 类声明）
- Create: `engine/renderer/PostProcess.cpp`

这是最大的 task，包含：资源创建/销毁、RenderPass、descriptor pool、管线、execute 逻辑。

- [ ] **Step 1: 在 PostProcess.h 中声明 PostProcessPipeline 类**

在已有的 `PostProcessSettings` 之后添加 class 声明（参见 spec 中的完整定义）。需要 `#include` vulkan 头文件和 forward declare VulkanContext、DescriptorLayoutCache。

- [ ] **Step 2: 创建 PostProcess.cpp — 初始化和销毁**

实现 `init()`:
1. 保存 context 指针和宽高
2. 创建 `m_linearSampler`（LINEAR filter, CLAMP_TO_EDGE）
3. 创建 descriptor set layout（binding 0 + binding 1 均为 COMBINED_IMAGE_SAMPLER）
4. 创建 `m_descriptorPool`（maxSets=16, combinedImageSampler×32）
5. 创建三个 RenderPass:
   - `m_bloomDownsampleRenderPass`：format=R16G16B16A16_SFLOAT, loadOp=DONT_CARE, finalLayout=SHADER_READ_ONLY
   - `m_bloomUpsampleRenderPass`：format=R16G16B16A16_SFLOAT, loadOp=LOAD, finalLayout=SHADER_READ_ONLY
   - `m_ldrRenderPass`：format=R8G8B8A8_UNORM, loadOp=DONT_CARE, finalLayout=SHADER_READ_ONLY
6. 调用 `createBloomResources()` 创建 bloom mip chain image/views/framebuffers
7. 创建 composite image/view/framebuffer（R8G8B8A8_UNORM，全分辨率）
8. 创建 fxaa image/view/framebuffer（R8G8B8A8_UNORM，全分辨率）
9. 创建四条管线（调用 createFullscreenBundlePipeline）:
   - 注意：后处理 shaderbundle 路径为 `ASSETS_DIR + "/shaders/postprocess/<Name>.shaderbundle"`，不使用 `shaderBundlePath()` 辅助函数（它只处理顶层目录）
   - bloom downsample: renderPass=bloomDownsampleRP, pushConstant=BloomPushConstant(24B)
   - bloom upsample: renderPass=bloomUpsampleRP, pushConstant=BloomPushConstant(24B), blendState=additive
   - composite: renderPass=ldrRP, pushConstant=CompositePushConstant(48B)
   - fxaa: renderPass=ldrRP, pushConstant=FxaaPushConstant(20B)
10. 为每个 pass 分配并写入 descriptor set
11. 创建一个 1×1 黑色 fallback texture（`m_blackFallback`），当 Bloom 关闭时 Composite pass 的 binding 1 绑定此 fallback（Vulkan 不允许 descriptor 为 null）

实现 `destroy()`: 按创建的逆序销毁所有资源。

实现 `resize()`: 销毁尺寸相关资源（images/views/framebuffers/descriptor sets），然后重建。RenderPass 和 pipeline 不需要重建。

- [ ] **Step 3: 实现 createBloomResources**

1. 计算 bloom mip chain 尺寸：mip 0 = width/2 × height/2，逐级减半
2. 创建单张 VkImage（mipLevels=MAX_BLOOM_MIPS, usage=COLOR_ATTACHMENT|SAMPLED）
3. 为每个 mip 创建 VkImageView（baseMipLevel=i, levelCount=1）
4. 为每个 mip 创建两个 VkFramebuffer（downsample RP + upsample RP）

- [ ] **Step 4: 实现 execute**

```cpp
void PostProcessPipeline::execute(VkCommandBuffer cmd, VkImageView sceneHDR,
                                   const PostProcessSettings& settings) {
    // 1. Bloom（如果启用）
    VkImageView bloomResult = VK_NULL_HANDLE;
    if (settings.bloomEnabled) {
        executeBloom(cmd, sceneHDR, settings);
        bloomResult = m_bloomMipViews[0];
    }

    // 2. Composite（始终执行）
    executeComposite(cmd, sceneHDR, bloomResult, settings);

    // 3. FXAA（如果启用）
    if (settings.fxaaEnabled) {
        executeFxaa(cmd, settings);
    }
}
```

- [ ] **Step 5: 实现 executeBloom**

逐步处理：
1. Bright extract: offscreen → mip 0（barrier + render pass + draw）
2. Downsample chain: mip 0 → mip 1 → ... → mip N-1（循环）
3. Upsample chain: mip N-1 → ... → mip 0（循环，additive blend，LoadOp=LOAD）

每一步前后做精确的 mip-level barrier。

- [ ] **Step 6: 实现 executeComposite**

1. Barrier: compositeImage UNDEFINED → COLOR_ATTACHMENT
2. Begin render pass (ldrRenderPass, compositeFramebuffer)
3. Bind pipeline, descriptor set (sceneHDR + bloom texture)
4. Push constant: 从 settings 填充 CompositePushConstant
5. vkCmdDraw(6, 1)
6. End render pass
7. Barrier: compositeImage → SHADER_READ_ONLY (下一步 FXAA 或 blit 使用)

- [ ] **Step 7: 实现 executeFxaa**

类似 executeComposite，输入 compositeImageView，输出 fxaaImage。

- [ ] **Step 8: 实现 getFinalImage/getFinalImageView**

```cpp
VkImage PostProcessPipeline::getFinalImage(const PostProcessSettings& s) const {
    return s.fxaaEnabled ? m_fxaaImage : m_compositeImage;
}
VkImageView PostProcessPipeline::getFinalImageView(const PostProcessSettings& s) const {
    return s.fxaaEnabled ? m_fxaaImageView : m_compositeImageView;
}
```

- [ ] **Step 9: 实现 reloadShaders**

销毁四条管线，重新调用 createFullscreenBundlePipeline。

- [ ] **Step 10: 编译验证**

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug
```

- [ ] **Step 11: 提交**

```bash
git add engine/renderer/PostProcess.h engine/renderer/PostProcess.cpp
git commit -m "feat(postprocess): implement PostProcessPipeline core"
```

---

## Task 8: Renderer 集成

**Files:**
- Modify: `engine/renderer/Renderer.h`
- Modify: `engine/renderer/Renderer.cpp`

- [ ] **Step 1: 在 Renderer.h 中添加成员和接口**

```cpp
#include "renderer/PostProcess.h"
// 在 private 区域 offscreen 资源之后添加:
PostProcessPipeline m_postProcess;
// 在 public 区域添加:
VkImageView getDisplayImageView(const Scene& scene) const;
VkImage getDisplayImage(const Scene& scene) const;
VkSampler getDisplaySampler() const;
```

- [ ] **Step 2: 在 Renderer::init 中初始化后处理**

在 `createOffscreen()` 调用之后添加：
```cpp
m_postProcess.init(m_context, m_layoutCache, m_offscreenWidth, m_offscreenHeight);
```

- [ ] **Step 3: 在 drawScene 中插入后处理调用**

在 `Renderer::drawScene()` 中，`drawSceneToOffscreen()` 和 `renderShadowPass()` 之后，方法返回之前：
```cpp
auto& ppSettings = scene.getPostProcessSettings();
m_postProcess.execute(cmd, m_offscreenImageView, ppSettings);
```

- [ ] **Step 4: 修改 blitToSwapchain**

将 `blitToSwapchain()` 中的 blit 源从 `m_offscreenImage` 改为后处理最终输出。

由于 blitToSwapchain 不接受参数，需要保存当前 scene 引用或在 drawScene 中保存 final image。最简方案：在 Renderer 中缓存 final image：

```cpp
// Renderer.h private:
VkImage m_displayImage = VK_NULL_HANDLE;
VkImageView m_displayImageView = VK_NULL_HANDLE;

// drawScene 中，execute 之后:
m_displayImage = m_postProcess.getFinalImage(ppSettings);
m_displayImageView = m_postProcess.getFinalImageView(ppSettings);
```

在 `blitToSwapchain()` 中进行以下完整替换：
1. 第 417 行 `offscreenBarrier.image = m_offscreenImage` → `offscreenBarrier.image = m_displayImage`
2. 第 449 行 `vkCmdBlitImage(cmd, m_offscreenImage, ...)` → `vkCmdBlitImage(cmd, m_displayImage, ...)`
3. blit 后 transition 回 SHADER_READ_ONLY 的 barrier 也需要作用于 `m_displayImage`（而非 m_offscreenImage）
4. 注意：后处理 execute 结束后，final image 已经处于 SHADER_READ_ONLY_OPTIMAL，所以 blit 前的 barrier（SHADER_READ_ONLY → TRANSFER_SRC）是正确的
5. blit 后将 display image 从 TRANSFER_SRC 转回 SHADER_READ_ONLY（供下一帧 SceneViewPanel 的 ImGui 采样使用）

- [ ] **Step 5: 实现 getDisplayImageView / getDisplayImage / getDisplaySampler**

```cpp
VkImageView Renderer::getDisplayImageView(const Scene& scene) const {
    return m_displayImageView != VK_NULL_HANDLE ? m_displayImageView : m_offscreenImageView;
}
VkImage Renderer::getDisplayImage(const Scene& scene) const {
    return m_displayImage != VK_NULL_HANDLE ? m_displayImage : m_offscreenImage;
}
VkSampler Renderer::getDisplaySampler() const {
    return m_offscreenSampler; // 后处理输出使用同一个 sampler
}
```

- [ ] **Step 6: 在 resizeOffscreen 中同步 resize**

在 `Renderer::resizeOffscreen()` 中添加：
```cpp
m_postProcess.resize(width, height);
```

- [ ] **Step 7: 在 reloadShaders 中添加后处理重载**

在 `Renderer::reloadShaders()` 末尾添加：
```cpp
m_postProcess.reloadShaders();
```

- [ ] **Step 8: 在 shutdown 中销毁**

在 `Renderer::shutdown()` 中添加：
```cpp
m_postProcess.destroy();
```

- [ ] **Step 9: 编译并运行验证**

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug
```

运行 QymEditor.exe，预期：
- 场景经过 Tone Mapping 和 FXAA 后显示
- HDR 高亮区域被正确 tone mapped（不会过曝白）
- 边缘更平滑（FXAA）

截图保存以确认效果。

- [ ] **Step 10: 提交**

```bash
git add engine/renderer/Renderer.h engine/renderer/Renderer.cpp
git commit -m "feat(renderer): integrate PostProcessPipeline into render loop"
```

---

## Task 9: SceneViewPanel 更新

**Files:**
- Modify: `editor/panels/SceneViewPanel.h`
- Modify: `editor/panels/SceneViewPanel.cpp:295-314`
- Modify: `editor/EditorApp.cpp` (applyPendingResize 调用点)

- [ ] **Step 1: 修改 SceneViewPanel.h 签名**

在 `SceneViewPanel.h` 中修改两个方法签名：
```cpp
// 旧：
void recreateDescriptorSet(Renderer& renderer);
void applyPendingResize(Renderer& renderer);
// 新：
void recreateDescriptorSet(Renderer& renderer, Scene& scene);
void applyPendingResize(Renderer& renderer, Scene& scene);
```

添加前向声明 `class Scene;`（或 include）。

- [ ] **Step 2: 修改 recreateDescriptorSet 实现**

在 `SceneViewPanel.cpp` 第 295-315 行：
```cpp
void SceneViewPanel::recreateDescriptorSet(Renderer& renderer, Scene& scene)
{
    VkImageView currentView = renderer.getDisplayImageView(scene);
    VkSampler   sampler     = renderer.getDisplaySampler();
    // ... 其余不变
}
```

- [ ] **Step 3: 修改 applyPendingResize 实现**

`applyPendingResize` 内部也调用了 `recreateDescriptorSet`，更新调用为 `recreateDescriptorSet(renderer, scene)`。

- [ ] **Step 4: 更新所有调用点**

在 `SceneViewPanel.cpp` 的 `onImGuiRender` 中更新：
```cpp
recreateDescriptorSet(renderer, scene);  // 已有 scene 参数
```

在 `EditorApp.cpp` 第 223 行更新：
```cpp
m_sceneViewPanel.applyPendingResize(m_renderer, m_scene);
```

- [ ] **Step 5: 编译并运行验证**

预期：编辑器视口显示后处理后的画面（tone mapped + FXAA）。

- [ ] **Step 6: 提交**

```bash
git add editor/panels/SceneViewPanel.h editor/panels/SceneViewPanel.cpp editor/EditorApp.cpp
git commit -m "feat(editor): update SceneViewPanel to display post-processed output"
```

---

## Task 10: PostProcessPanel Inspector 面板

**Files:**
- Create: `editor/panels/PostProcessPanel.h`
- Create: `editor/panels/PostProcessPanel.cpp`
- Modify: `editor/EditorApp.h`
- Modify: `editor/EditorApp.cpp:403-408`

- [ ] **Step 1: 创建 PostProcessPanel.h**

```cpp
#pragma once
#include "scene/Scene.h"

namespace QymEngine {

class PostProcessPanel {
public:
    void onImGuiRender(Scene& scene);
};

} // namespace QymEngine
```

- [ ] **Step 2: 创建 PostProcessPanel.cpp**

```cpp
#include "PostProcessPanel.h"
#include <imgui.h>

namespace QymEngine {

void PostProcessPanel::onImGuiRender(Scene& scene) {
    ImGui::Begin("后处理");
    auto& pp = scene.getPostProcessSettings();

    // Bloom
    if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用##bloom", &pp.bloomEnabled);
        if (pp.bloomEnabled) {
            ImGui::DragFloat("阈值", &pp.bloomThreshold, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("强度##bloom", &pp.bloomIntensity, 0.01f, 0.0f, 5.0f);
            ImGui::SliderInt("Mip 级数", &pp.bloomMipCount, 1, MAX_BLOOM_MIPS);
        }
    }

    // Tone Mapping
    if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用##tonemapping", &pp.toneMappingEnabled);
        ImGui::DragFloat("曝光", &pp.exposure, 0.01f, 0.001f, 20.0f);
    }

    // Color Grading
    if (ImGui::CollapsingHeader("Color Grading", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用##colorgrading", &pp.colorGradingEnabled);
        if (pp.colorGradingEnabled) {
            ImGui::SliderFloat("对比度", &pp.contrast, 0.5f, 2.0f);
            ImGui::SliderFloat("饱和度", &pp.saturation, 0.0f, 2.0f);
            ImGui::SliderFloat("色温", &pp.temperature, -1.0f, 1.0f);
            ImGui::SliderFloat("色调", &pp.tint, -1.0f, 1.0f);
            ImGui::SliderFloat("亮度", &pp.brightness, -1.0f, 1.0f);
        }
    }

    // FXAA
    if (ImGui::CollapsingHeader("FXAA", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用##fxaa", &pp.fxaaEnabled);
        if (pp.fxaaEnabled) {
            ImGui::SliderFloat("子像素质量", &pp.fxaaSubpixQuality, 0.0f, 1.0f);
            ImGui::SliderFloat("边缘阈值", &pp.fxaaEdgeThreshold, 0.063f, 0.333f);
            ImGui::SliderFloat("最小阈值", &pp.fxaaEdgeThresholdMin, 0.0312f, 0.0833f);
        }
    }

    pp.clampValues();
    ImGui::End();
}

} // namespace QymEngine
```

- [ ] **Step 3: 注册面板到 EditorApp**

在 `EditorApp.h` 中添加：
```cpp
#include "panels/PostProcessPanel.h"
// 成员:
PostProcessPanel m_postProcessPanel;
```

在 `EditorApp.cpp` 第 408 行 `m_consolePanel.onImGuiRender()` 之后添加：
```cpp
m_postProcessPanel.onImGuiRender(m_scene);
```

- [ ] **Step 4: 编译并运行验证**

预期：编辑器中出现"后处理"面板，可以实时调节所有参数并看到效果变化。

- [ ] **Step 5: 提交**

```bash
git add editor/panels/PostProcessPanel.h editor/panels/PostProcessPanel.cpp editor/EditorApp.h editor/EditorApp.cpp
git commit -m "feat(editor): add PostProcessPanel for runtime parameter tuning"
```

---

## Task 11: 独立运行时集成

**Files:**
- Modify: `QymEngine/main.cpp:42-44`

- [ ] **Step 1: 在独立运行时中也启用后处理**

当前独立运行时流程（QymEngine/main.cpp:42-44）：
```cpp
m_renderer.drawScene(m_scene);
m_renderer.blitToSwapchain();
```

由于后处理已在 `Renderer::drawScene()` 内部调用，且 `blitToSwapchain()` 已改为 blit 后处理输出，独立运行时无需额外修改，自动生效。

验证：运行 QymEngine.exe，确认后处理效果正常。

- [ ] **Step 2: 提交（如有修改）**

如果独立运行时需要调整，提交相应修改。

---

## Task 12: 最终验证与清理

- [ ] **Step 1: 全量编译**

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug
```

- [ ] **Step 2: 编辑器功能验证**

运行 QymEditor.exe 测试以下场景：
1. 加载默认场景 → 场景正常渲染，有 Bloom 和 Tone Mapping 效果
2. 在后处理面板中关闭 Bloom → Bloom 消失，其余效果正常
3. 调节曝光值 → 画面亮度变化
4. 调节对比度/饱和度/色温 → 颜色变化
5. 关闭 FXAA → 边缘锯齿变明显
6. 全部关闭 → 仍有 HDR→LDR 转换和 gamma 矫正
7. 调整视口大小 → 后处理资源正确 resize

- [ ] **Step 3: 截图对比**

截取开启/关闭后处理的对比截图，确认效果正确。

- [ ] **Step 4: 提交最终状态**

```bash
git add -A
git commit -m "feat: complete post-processing system (Bloom + ToneMapping + ColorGrading + FXAA)"
```
