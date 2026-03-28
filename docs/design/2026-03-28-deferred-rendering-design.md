# 延迟渲染管线设计

## 背景

当前引擎使用前向渲染，光照在场景 pass 的 fragment shader 中逐物体计算。这限制了屏幕空间效果（SSAO、SSR）的实现，因为没有 G-Buffer 提供 depth/normal 等几何信息。

延迟渲染将几何信息输出到 G-Buffer，光照在全屏 pass 中统一计算，为后续屏幕空间效果提供基础。

## G-Buffer 布局（精简 3-RT）

| 附件 | 格式 | 内容 |
|------|------|------|
| RT0 | RGBA8 | Albedo.rgb + Metallic |
| RT1 | RG16F | Normal.xy（八面体编码） |
| RT2 | RGBA8 | Roughness + AO + 保留 + 保留 |
| Depth | D32F | 硬件深度缓冲 |

- 世界坐标从深度缓冲 + 逆 VP 矩阵重建
- 八面体法线编码：2 个 float16 存储单位法线，精度足够
- 总带宽 ~12 bytes/pixel

## 渲染流程

```
前向模式（保留）:
  Shadow Pass → Scene Pass (Lit) → Post-Process → Composite → SwapChain

延迟模式（新增）:
  Shadow Pass → G-Buffer Pass → Lighting Pass → Post-Process → Composite → SwapChain
```

Shadow pass、后处理链（Bloom/DOF/FXAA/Composite）不变。

## 新增着色器

### GBuffer.slang

G-Buffer 几何 pass。顶点变换复用 Lit 结构，fragment shader 输出材质信息到 3 个 RT：

```
VSOutput: worldPos, normal, texCoord, metallic, roughness
Fragment:
  RT0 = vec4(albedo, metallic)
  RT1 = octEncode(normalize(normal))
  RT2 = vec4(roughness, ao, 0, 0)
```

### DeferredLighting.slang

全屏 quad lighting pass。读取 G-Buffer + 深度重建位置，执行 PBR 光照：

输入：
- G-Buffer 3 个 RT + 深度纹理
- Shadow map + FrameData UBO（光源数据、VP 矩阵）
- IBL cubemaps（irradiance + prefiltered）+ BRDF LUT

光照逻辑从 Lit.slang 提取：
- 多光源遍历（方向光、点光、聚光灯）
- PCF 阴影采样
- IBL 漫反射 + 镜面反射

输出：HDR color (R16G16B16A16F)

### SSAO.slang

在 Lighting pass 之前执行，采样 G-Buffer depth + normal 生成 AO 纹理。

提供三种算法（push constant 切换）：
- 经典 SSAO：64 半球采样 + 4x4 噪声纹理
- HBAO：多方向光线步进 + 地平线角度
- GTAO：余弦加权解析积分

输出 AO 值写入单通道纹理，Lighting pass 的 ambient 项乘以 AO。

## Renderer 改动

### 新增资源
- G-Buffer images（3 个 color + depth）+ image views + sampler
- G-Buffer render pass（3 color + 1 depth attachment，clear 后写入）
- G-Buffer framebuffer
- Lighting pass render pass + framebuffer（输出到 offscreen HDR target）
- SSAO 纹理（R8 或 R16F） + render pass + framebuffer
- 噪声纹理（4x4 随机向量）

### 新增 Pipeline
- GBuffer pipeline（GBuffer.slang，3 个 color attachment）
- DeferredLighting pipeline（全屏 quad，读 G-Buffer）
- SSAO pipeline（全屏 quad，读 depth + normal）
- SSAO blur pipeline（可选，3x3 或双边模糊）

### drawScene 分发
```cpp
if (m_deferredEnabled) {
    renderShadowPass(cmd, scene);
    renderGBufferPass(cmd, scene);   // 新增
    renderSSAOPass(cmd);              // 新增
    renderLightingPass(cmd);          // 新增
    renderPostProcess(cmd);           // 不变
} else {
    renderShadowPass(cmd, scene);
    renderScenePass(cmd, scene);      // 现有前向
    renderPostProcess(cmd);
}
```

### 资源生命周期
- G-Buffer 资源在 `createOffscreen()` 时创建，`destroyOffscreen()` 时销毁
- 随 offscreen resize 重建（与现有 bloom mip chain 相同的模式）

## 编辑器集成

Inspector 面板（或 PostProcess 面板）添加：
- Rendering Mode 下拉框：Forward / Deferred
- SSAO 开关 + 算法选择（Classic / HBAO / GTAO）
- SSAO 参数：radius、bias、intensity

切换渲染模式时重建相关 pipeline。

## 多后端兼容

| 后端 | MRT 支持 | RG16F 支持 | 备注 |
|------|---------|-----------|------|
| Vulkan | 8 RT | 原生 | 无限制 |
| D3D12 | 8 RT | 原生 | 无限制 |
| D3D11 | 8 RT | 原生 | 无限制 |
| OpenGL 4.5 | 8 RT | 原生 | 无限制 |
| GLES 3.0 | 4 RT | 需要 EXT_color_buffer_half_float | 3 RT + depth 刚好在限制内 |

GLES 3.0 上需检查 `EXT_color_buffer_half_float` 扩展，不支持时 RT1 改用 RGBA8（法线精度降低但可用）。

## 不做

- 透明物体渲染（当前场景无透明物体，后续需要时加前向透明 pass）
- Deferred Decals
- 屏幕空间反射（SSR）— 延迟渲染架构就绪后可作为后续功能
- G-Buffer 可视化调试工具（可后续加）
