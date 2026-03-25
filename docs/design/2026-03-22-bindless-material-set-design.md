# Bindless Material Set 设计

## 目标

将 per-material descriptor set (set 1) 改为 bindless 架构：Sampler 数组 + Texture 数组 + MaterialEntry SSBO (含 BDA + 贴图索引)。仅 PC 启用，Android fallback 到当前方案。Shader 通过编译选项切换。

## Set 1 布局

### Bindless (PC)

```
Set 1:
  binding 0: Sampler[]         (256, VK_DESCRIPTOR_TYPE_SAMPLER)
  binding 1: Texture2D[]       (unbounded, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
  binding 2: StorageBuffer     (MaterialEntry SSBO)
```

MaterialEntry SSBO:
```glsl
struct MaterialEntry {
    uint64_t paramsAddr;     // BDA → material params buffer
    uint albedoTexIndex;
    uint normalTexIndex;
    uint samplerIndex;
};
```

Material params buffer (每个材质独立, 通过 BDA 访问):
```glsl
struct LitMaterialParams {
    vec4 baseColor;
    float metallic;
    float roughness;
};
```

### Non-bindless (Android, fallback)

保持当前方案不变：
```
Set 1:
  binding 0: UniformBuffer (MaterialParams)
  binding 1: CombinedImageSampler (albedoMap)
  binding 2: CombinedImageSampler (normalMap)
```

## Push Constants

```cpp
struct PushConstantData {
    glm::mat4 model;       // 64 bytes
    int highlighted;        // 4 bytes
    uint32_t materialIndex; // 4 bytes (bindless only, ignored in non-bindless)
    int _pad[2];            // 8 bytes padding
};                          // total: 80 bytes
```

## Shader 编译

Shader compiler 对每个 .slang 编译两套 SPIR-V：
- 无 define → 普通 .spv (non-bindless)
- `USE_BINDLESS=1` → `*_bindless.spv` (bindless)

同时生成两套 .reflect.json。

### Slang Shader 示例 (Lit.slang)

```slang
#ifdef USE_BINDLESS
// Bindless path
[[vk::binding(0, 1)]] SamplerState samplers[];
[[vk::binding(1, 1)]] Texture2D textures[];

struct MaterialEntry {
    uint64_t paramsAddr;
    uint albedoTexIndex;
    uint normalTexIndex;
    uint samplerIndex;
};
[[vk::binding(2, 1)]] StructuredBuffer<MaterialEntry> materialEntries;

struct PushConstants {
    float4x4 model;
    int highlighted;
    uint materialIndex;
};

#else
// Non-bindless path (current)
struct LitMaterialParams {
    float4 baseColor;
    float metallic;
    float roughness;
};
[[vk::binding(0, 1)]] ConstantBuffer<LitMaterialParams> materialParams;
[[vk::binding(1, 1)]] Sampler2D albedoMap;
[[vk::binding(2, 1)]] Sampler2D normalMap;

struct PushConstants {
    float4x4 model;
    int highlighted;
};
#endif
```

## Vulkan Feature 启用

VulkanContext 设备创建时 (PC only):
- 查询 `VkPhysicalDeviceDescriptorIndexingFeatures`
- 查询 `VkPhysicalDeviceBufferDeviceAddressFeatures`
- 启用: `descriptorBindingPartiallyBound`, `runtimeDescriptorArray`, `shaderSampledImageArrayNonUniformIndexing`, `bufferDeviceAddress`
- 暴露 `bool supportsBindless()` 给 Renderer

## 运行时流程

### 初始化 (Renderer)

1. 检查 `VulkanContext::supportsBindless()`
2. 若支持:
   - 创建 bindless set 1 layout (sampler[256] + texture[unbounded] + SSBO)
   - 分配一个全局 bindless descriptor set
   - 创建全局 MaterialEntry SSBO
   - 创建默认 sampler 注册到 samplers[0]
3. Pipeline::create() 加载 `*_bindless.spv`

### 材质加载 (AssetManager)

1. 加载贴图 → 注册到全局 texture 数组，获取 index
2. 创建材质 params buffer (baseColor/metallic/roughness)，获取 BDA
3. 填充 MaterialEntry {paramsAddr, albedoIdx, normalIdx, samplerIdx}
4. 更新 SSBO

### 渲染 (Renderer::drawSceneToOffscreen)

```cpp
// Bindless: 绑定一次 set 1, 每个 drawcall 只改 push constant
vkCmdBindDescriptorSets(..., bindlessSet1, ...);
for each node:
    pc.materialIndex = node->materialIndex;
    vkCmdPushConstants(...);
    draw();
```

## 实现模块

1. **VulkanContext** — feature chain 查询/启用, supportsBindless()
2. **Shader compiler** — 双路编译 (USE_BINDLESS define)
3. **Slang shaders** — #ifdef USE_BINDLESS 两条路径
4. **PushConstantData** — 加 materialIndex
5. **Renderer** — bindless set 1 创建, 全局 texture/sampler 注册
6. **AssetManager** — 材质加载走 bindless 或 fallback
7. **Pipeline** — 根据 bindless 选择 .spv
