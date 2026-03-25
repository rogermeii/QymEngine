# 反射驱动管线 + 材质系统设计

## 目标

1. 消除 descriptor set layout handle 不匹配问题，用 DescriptorLayoutCache 统一管理 layout 生命周期
2. Pipeline 完全由 shader 反射数据驱动创建，删除 `createWithLayouts()` workaround
3. 按更新频率分组 descriptor set：set 0 (per-frame) / set 1 (per-material) / push constant (per-object)
4. 材质参数从 push constant 迁移到 set 1 的 UBO，不同 shader 可以有不同的 set 1 布局
5. 编辑器中支持完整材质编辑：属性编辑、shader 切换、创建/保存材质

## 架构总览

```
Slang Shader (.slang)
    ↓ [Shader Compiler]
SPIR-V (.spv) + Reflection (.reflect.json)
    ↓ [Runtime]
DescriptorLayoutCache ← 反射数据驱动 layout 创建/去重
    ↓
Pipeline (从反射 + cache 创建)
Descriptor (从 cache 获取 layout, 分配 set)
MaterialInstance (set 1 descriptor set + param UBO)
```

## 1. Descriptor Set 分组

### Set 0: Per-Frame（全局，每帧绑定一次）

```
binding 0: UniformBuffer — FrameData { view, proj, lightDir, lightColor, ambientColor, cameraPos }
```

所有 shader 必须使用相同的 set 0 布局，引擎强制约定。启动时注册到 cache，shader 反射的 set 0 必须与之匹配，否则报错。

### Set 1: Per-Material（材质切换时绑定）

由 shader 反射决定，不同 shader 可以有不同布局。通过 Slang ParameterBlock 机制声明。

示例 — Lit shader:
```
binding 0: UniformBuffer — MaterialParams { baseColor: vec4, metallic: float, roughness: float }
binding 1: CombinedImageSampler — albedoMap
binding 2: CombinedImageSampler — normalMap
```

示例 — Unlit shader:
```
binding 0: UniformBuffer — MaterialParams { baseColor: vec4 }
binding 1: CombinedImageSampler — albedoMap
```

### Push Constants: Per-Object（每个 drawcall）

```cpp
struct PushConstantData {
    glm::mat4 model;    // 64 bytes
    int highlighted;     // 4 bytes
    int _pad[3];         // 12 bytes padding
};                       // total: 80 bytes
```

材质属性（baseColor, metallic, roughness）从 push constant 移到 set 1 的 MaterialParams UBO。

## 2. DescriptorLayoutCache

### 职责

统一管理所有 VkDescriptorSetLayout 的创建、去重和销毁。

### 接口

```cpp
class DescriptorLayoutCache {
public:
    // 结构相同的 bindings 返回同一个 layout handle
    VkDescriptorSetLayout getOrCreate(VkDevice device,
        const std::vector<VkDescriptorSetLayoutBinding>& bindings);

    void cleanup(VkDevice device);

private:
    struct LayoutKey {
        // binding index + descriptor type + stage flags + count 的排序组合
        bool operator==(const LayoutKey& other) const;
    };
    struct LayoutKeyHash { size_t operator()(const LayoutKey& k) const; };

    std::unordered_map<LayoutKey, VkDescriptorSetLayout, LayoutKeyHash> m_cache;
};
```

### 使用方

- **Pipeline::create()**：从反射 JSON 构造 bindings → 调用 cache.getOrCreate()
- **Descriptor**：分配 set 时从 cache 获取 layout
- **AssetManager**：材质 shader 的 pipeline 创建
- **Renderer**：启动时注册 set 0 的固定 layout

### 生命周期

Renderer 持有 DescriptorLayoutCache 唯一实例。引擎关闭时 Renderer::shutdown() 中调用 cache.cleanup()，统一销毁所有 layout。Pipeline 和 Descriptor 不再销毁 layout。

## 3. Pipeline 改造

### 删除 createWithLayouts()

统一使用 `create()` 方法：

```cpp
void Pipeline::create(VkDevice device, VkRenderPass renderPass,
                      VkExtent2D extent, DescriptorLayoutCache& layoutCache,
                      VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
                      const std::string& vertPath = "",
                      const std::string& fragPath = "");
```

流程：
1. 加载 .spv 文件
2. 加载 .reflect.json（从 spv 路径推导）
3. 对每个 set，构造 VkDescriptorSetLayoutBinding → layoutCache.getOrCreate()
4. 从反射构造 push constant ranges
5. 创建 pipeline layout + graphics pipeline

### 不再拥有 layout

Pipeline 只持有 cache 返回的 layout handle 引用（用于 getPipelineLayout()），不在 cleanup() 中销毁 layout。

## 4. Descriptor 改造

### 删除固定 layout 创建

删除 `createUboLayout()` 和 `createTextureLayout()`。

### 新增通用分配方法

```cpp
class Descriptor {
public:
    void createPool(VkDevice device, int maxFramesInFlight, int maxMaterials = 100);

    // 通用 set 分配
    VkDescriptorSet allocateSet(VkDevice device, VkDescriptorSetLayout layout);

    // Per-frame UBO set 分配（引擎内部用）
    void createPerFrameSets(VkDevice device, int maxFramesInFlight,
                            VkDescriptorSetLayout perFrameLayout,
                            const std::vector<VkBuffer>& uniformBuffers);

    VkDescriptorSet getPerFrameSet(uint32_t frame) const;
    VkDescriptorPool getPool() const;

    void cleanup(VkDevice device);

private:
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_perFrameSets;
};
```

## 5. Slang Shader ParameterBlock

### Lit.slang 示例

```slang
struct FrameData {
    float4x4 view;
    float4x4 proj;
    float3 lightDir;
    float3 lightColor;
    float3 ambientColor;
    float3 cameraPos;
};

struct LitMaterialParams {
    float4 baseColor;
    float metallic;
    float roughness;
};

struct PushConstants {
    float4x4 model;
    int highlighted;
};

// Set 0: per-frame (engine-managed, all shaders must match)
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;

// Set 1: per-material (shader-specific layout)
[[vk::binding(0, 1)]] ConstantBuffer<LitMaterialParams> materialParams;
[[vk::binding(1, 1)]] Sampler2D albedoMap;
[[vk::binding(2, 1)]] Sampler2D normalMap;

// Push constant: per-object
[[vk::push_constant]] PushConstants pc;
```

UBO struct 中的 sampler 不混合声明，sampler 用独立的 `[[vk::binding]]` 声明。这确保 binding 编号与引擎约定精确匹配。

### Grid.slang

Grid shader 只使用 set 0（无材质），不声明 set 1 binding。

## 6. Shader Compiler 改造

### .reflect.json 增强

新增 `members` 字段（UBO 和 push constant 内部成员信息）和 `size` 字段：

```json
{
  "sets": [
    {
      "set": 0,
      "name": "frame",
      "bindings": [
        {
          "binding": 0, "type": "uniformBuffer", "name": "FrameData",
          "size": 192,
          "members": [
            {"name": "view", "type": "float4x4", "offset": 0},
            {"name": "proj", "type": "float4x4", "offset": 64},
            {"name": "lightDir", "type": "float3", "offset": 128},
            {"name": "lightColor", "type": "float3", "offset": 144},
            {"name": "ambientColor", "type": "float3", "offset": 160},
            {"name": "cameraPos", "type": "float3", "offset": 176}
          ],
          "stages": ["vertex", "fragment"]
        }
      ]
    },
    {
      "set": 1,
      "name": "material",
      "bindings": [
        {
          "binding": 0, "type": "uniformBuffer", "name": "MaterialParams",
          "size": 32,
          "members": [
            {"name": "baseColor", "type": "float4", "offset": 0},
            {"name": "metallic", "type": "float", "offset": 16},
            {"name": "roughness", "type": "float", "offset": 20}
          ],
          "stages": ["fragment"]
        },
        {"binding": 1, "type": "combinedImageSampler", "name": "albedoMap", "stages": ["fragment"]},
        {"binding": 2, "type": "combinedImageSampler", "name": "normalMap", "stages": ["fragment"]}
      ]
    }
  ],
  "pushConstants": [
    {
      "offset": 0, "size": 80, "stages": ["vertex"],
      "members": [
        {"name": "model", "type": "float4x4", "offset": 0},
        {"name": "highlighted", "type": "int", "offset": 64}
      ]
    }
  ]
}
```

### SPIRV-Reflect 增强

使用 SPIRV-Reflect 的 member reflection API 提取 UBO 内部成员的 name、type、offset、size。现有的 shader compiler 已有 SPIRV-Reflect 集成，需要扩展反射深度。

## 7. 运行时材质系统

### MaterialInstance

```cpp
struct MaterialInstance {
    std::string name;
    std::string shaderPath;
    ShaderAsset* shader = nullptr;

    // Set 1 descriptor set（从 shader 反射的 layout 分配）
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    // MaterialParams UBO
    VkBuffer paramBuffer = VK_NULL_HANDLE;
    VkDeviceMemory paramMemory = VK_NULL_HANDLE;
    void* paramMapped = nullptr;
    uint32_t paramBufferSize = 0;

    // 属性值（按反射的 member 名索引）
    std::map<std::string, glm::vec4> vec4Props;
    std::map<std::string, float> floatProps;
    std::map<std::string, std::string> texturePaths;
};
```

### 创建流程

1. 加载 shader → 获取 set 1 的反射信息（layout + members）
2. 从 DescriptorLayoutCache 获取 set 1 layout
3. 从 Descriptor pool 分配 descriptor set
4. 创建 MaterialParams UBO（大小从反射的 `size` 字段获取）
5. 加载贴图 → 写入 descriptor set 的 sampler bindings
6. 从 .mat.json 读取属性默认值 → 写入 paramBuffer

### 属性更新

修改属性时：
- float/vec4 属性：直接写入 paramMapped 对应 offset（从反射 members 查找）
- texture 属性：加载新贴图 → vkUpdateDescriptorSets 更新对应 binding

### .mat.json 格式

```json
{
  "name": "Red Metal",
  "shader": "shaders/standard_lit.shader.json",
  "properties": {
    "baseColor": [1.0, 0.2, 0.2, 1.0],
    "metallic": 0.8,
    "roughness": 0.3,
    "albedoMap": "textures/metal_albedo.jpg",
    "normalMap": "textures/metal_normal.jpg"
  }
}
```

## 8. 编辑器材质编辑

### Inspector 面板

选中带材质的节点时，Inspector 显示：

1. **Shader 选择** — 下拉菜单，列出所有 .shader.json 文件。切换时重建 MaterialInstance（新 layout, 新 descriptor set, 新 param UBO）
2. **属性编辑** — 根据 shader 反射的 members 动态生成：
   - `float4` → ImGui::ColorEdit4
   - `float` → ImGui::DragFloat（如果 shader.json 中有 range 则限制范围）
   - `Sampler2D` → 贴图缩略图 + "Browse" 按钮 / 拖拽替换
3. **操作按钮**
   - "New Material" — 基于当前 shader 创建新材质实例
   - "Save Material" — 序列化为 .mat.json
   - "Save As" — 另存为新文件

### 实时预览

属性修改直接写入 paramBuffer（persistent mapped），下一帧即生效，无需重建 descriptor set（除非切换贴图）。

## 9. 渲染流程变化

### 之前

```
foreach node:
    bind pipeline (per-shader)
    bind set 0 (UBO, Descriptor's layout)
    bind set 1 (textures, Descriptor's layout)
    push constants (model + baseColor + metallic + roughness + highlighted)
    draw
```

### 之后

```
bind set 0 once per frame (FrameData)
foreach node:
    bind pipeline (per-shader, from reflection)
    bind set 1 (material descriptor set, shader-specific layout)
    push constants (model + highlighted only)
    draw
```

关键变化：
- Set 0 只绑定一次（在 render pass 开始时）
- Set 1 来自 MaterialInstance，不同 shader 的 set 1 布局可以不同
- Push constant 只有 80 bytes（model + highlighted + padding）

## 10. 补充说明

### Shader 声明方式：显式 binding 注解

Shader 继续使用 `[[vk::binding(N, M)]]` 显式注解（而非依赖 Slang ParameterBlock 自动分配），确保 set/binding 与引擎约定精确匹配。ParameterBlock 的概念体现在分组上（set 0 = frame, set 1 = material），但 binding 编号由 shader 作者显式控制。

示例（Lit.slang）：
```slang
// Set 0: per-frame (engine-managed)
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;

// Set 1: per-material (shader-specific)
[[vk::binding(0, 1)]] ConstantBuffer<LitMaterialParams> materialParams;
[[vk::binding(1, 1)]] Sampler2D albedoMap;
[[vk::binding(2, 1)]] Sampler2D normalMap;

// Push constant: per-object
[[vk::push_constant]] PushConstants pc;
```

### Push Constant Stage Flags

新设计中 push constant 只含 model (mat4) 和 highlighted (int)，这些数据在 vertex shader 中使用并通过 varying 传到 fragment shader。因此 push constant 的 stageFlags 为 `VK_SHADER_STAGE_VERTEX_BIT`，与反射一致。

### MaterialInstance UBO 帧同步

MaterialInstance 的 paramBuffer 使用单 buffer（不做 per-frame 多缓冲）。材质参数变化不频繁（仅编辑器修改时），且修改发生在 CPU 端 `vkQueueWaitIdle` 后或两帧之间自然间隔中，单缓冲可接受。

### MaterialInstance 取代 MaterialAsset

`MaterialInstance` 取代现有的 `MaterialAsset`。`ShaderAsset` 保留，代表共享的 shader pipeline + 反射数据。多个 MaterialInstance 可引用同一个 ShaderAsset。

### MaterialInstance 生命周期

AssetManager 拥有所有 MaterialInstance。`AssetManager::shutdown()` 中销毁所有 MaterialInstance 的 paramBuffer / paramMemory。Descriptor set 不需要单独释放（随 pool 销毁）。

### Wireframe Pipeline

Wireframe pipeline 使用与 offscreen pipeline 相同的 shader（Lit），因此共享相同的反射数据和 cache 中的 layout。通过 `Pipeline::create()` + LayoutCache 创建，仅 polygonMode 不同。

### Descriptor Pool 配置

```
VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    maxFramesInFlight (set 0 per-frame)
  + maxMaterials (set 1 per-material param UBO)

VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    maxMaterials * 4 (每个材质最多 4 个贴图槽)

maxSets = maxFramesInFlight + maxMaterials
```

### Sampler Binding 与属性名映射

Sampler 属性名从 .reflect.json 的 set 1 binding 的 `"name"` 字段获取（如 `"albedoMap"`, `"normalMap"`）。编辑器遍历 set 1 bindings，type 为 `combinedImageSampler` 的 binding 的 `name` 即为属性名，与 .mat.json 中 properties 的 key 对应。

### Set 0 验证

Pipeline::create() 加载反射后，将 set 0 的 bindings 与引擎预注册的固定 set 0 layout 做 binding-by-binding 比较（binding index + descriptor type + stage flags）。不匹配则输出错误日志并跳过该 shader 加载。

### DescriptorLayoutCache 依赖注入

Renderer 持有 DescriptorLayoutCache。传递路径：
- `Pipeline::create()` 接受 `DescriptorLayoutCache&` 参数
- `AssetManager::setLayoutCache(DescriptorLayoutCache*)` — Renderer 初始化时调用
- AssetManager 在 `loadShader()` 中将 cache 传给 `Pipeline::create()`

### 迁移兼容

- 现有 .shader.json 和 .mat.json 格式保持向后兼容
- Grid shader 特殊处理：只用 set 0，不使用 set 1（pipeline 反射出 0 个 set 1 binding）
- 迁移分两步：(1) 引擎侧改造 — DescriptorLayoutCache + Pipeline + Descriptor + MaterialInstance，shader 保持 `[[vk::binding]]` 不变；(2) Shader 侧改造 — 将材质参数从 push constant 移到 set 1 ConstantBuffer，更新 shader compiler 产出增强的 .reflect.json
