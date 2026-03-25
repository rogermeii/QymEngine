# Reflection-Driven Pipeline + Material System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace hardcoded descriptor set layouts with a reflection-driven DescriptorLayoutCache, move material properties from push constants to per-material descriptor sets, and build a complete material editing system in the editor.

**Architecture:** DescriptorLayoutCache provides deduplicated VkDescriptorSetLayout handles. Pipeline::create() uses reflection JSON + cache instead of explicit layouts. MaterialInstance holds per-material UBO + descriptor set (set 1). Push constants reduced to model matrix + highlighted flag only. Inspector panel dynamically generates UI from shader reflection.

**Tech Stack:** C++17, Vulkan 1.4, Slang (shader language), SPIRV-Reflect, nlohmann/json, ImGui

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `engine/renderer/DescriptorLayoutCache.h` | Create | Layout deduplication cache |
| `engine/renderer/DescriptorLayoutCache.cpp` | Create | Cache implementation |
| `engine/renderer/Pipeline.h` | Modify | Add LayoutCache param to create(), remove createWithLayouts() |
| `engine/renderer/Pipeline.cpp` | Modify | Use cache for layout creation |
| `engine/renderer/Descriptor.h` | Modify | Remove fixed layouts, add generic allocateSet() |
| `engine/renderer/Descriptor.cpp` | Modify | Rewrite to use external layouts |
| `engine/renderer/Buffer.h` | Modify | Shrink PushConstantData to model+highlighted |
| `engine/renderer/Renderer.h` | Modify | Add DescriptorLayoutCache member |
| `engine/renderer/Renderer.cpp` | Modify | New init/draw flow with cache + MaterialInstance |
| `engine/asset/AssetManager.h` | Modify | MaterialInstance replaces MaterialAsset, add cache ref |
| `engine/asset/AssetManager.cpp` | Modify | New loadShader/loadMaterial using cache + param UBO |
| `engine/asset/MaterialAsset.h` | Delete/Replace | Replaced by MaterialInstance in AssetManager.h |
| `editor/panels/InspectorPanel.cpp` | Modify | Dynamic material UI from reflection |
| `tools/shader_compiler/main.cpp` | Modify | Add members/size to reflect.json |
| `assets/shaders/Lit.slang` | Modify | Move material params to set 1 ConstantBuffer |
| `assets/shaders/Triangle.slang` | Modify | Same as Lit |
| `assets/shaders/Unlit.slang` | Modify | Same pattern |
| `assets/shaders/Grid.slang` | Modify | Rename UBO to FrameData (set 0 only) |

---

### Task 1: Create DescriptorLayoutCache

**Files:**
- Create: `engine/renderer/DescriptorLayoutCache.h`
- Create: `engine/renderer/DescriptorLayoutCache.cpp`
- Modify: `engine/CMakeLists.txt` (add new source files)

- [ ] **Step 1: Create DescriptorLayoutCache.h**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>

namespace QymEngine {

class DescriptorLayoutCache {
public:
    // Returns existing layout if bindings match, otherwise creates new
    VkDescriptorSetLayout getOrCreate(VkDevice device,
        const std::vector<VkDescriptorSetLayoutBinding>& bindings);

    void cleanup(VkDevice device);

private:
    struct LayoutKey {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bool operator==(const LayoutKey& other) const;
    };

    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& key) const;
    };

    std::unordered_map<LayoutKey, VkDescriptorSetLayout, LayoutKeyHash> m_cache;
};

} // namespace QymEngine
```

- [ ] **Step 2: Create DescriptorLayoutCache.cpp**

```cpp
#include "renderer/DescriptorLayoutCache.h"
#include <algorithm>
#include <stdexcept>

namespace QymEngine {

bool DescriptorLayoutCache::LayoutKey::operator==(const LayoutKey& other) const {
    if (bindings.size() != other.bindings.size()) return false;
    for (size_t i = 0; i < bindings.size(); i++) {
        if (bindings[i].binding != other.bindings[i].binding) return false;
        if (bindings[i].descriptorType != other.bindings[i].descriptorType) return false;
        if (bindings[i].descriptorCount != other.bindings[i].descriptorCount) return false;
        if (bindings[i].stageFlags != other.bindings[i].stageFlags) return false;
    }
    return true;
}

size_t DescriptorLayoutCache::LayoutKeyHash::operator()(const LayoutKey& key) const {
    size_t hash = key.bindings.size();
    for (auto& b : key.bindings) {
        size_t bHash = b.binding | (b.descriptorType << 8) |
                       (b.stageFlags << 16) | (b.descriptorCount << 24);
        hash ^= std::hash<size_t>()(bHash);
    }
    return hash;
}

VkDescriptorSetLayout DescriptorLayoutCache::getOrCreate(VkDevice device,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings)
{
    LayoutKey key;
    key.bindings = bindings;
    // Sort by binding index for consistent hashing
    std::sort(key.bindings.begin(), key.bindings.end(),
        [](const auto& a, const auto& b) { return a.binding < b.binding; });

    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return it->second;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(key.bindings.size());
    layoutInfo.pBindings = key.bindings.data();

    VkDescriptorSetLayout layout;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("failed to create descriptor set layout in cache!");

    m_cache[key] = layout;
    return layout;
}

void DescriptorLayoutCache::cleanup(VkDevice device) {
    for (auto& [key, layout] : m_cache)
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
    m_cache.clear();
}

} // namespace QymEngine
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `renderer/DescriptorLayoutCache.cpp` to the engine library source list in `engine/CMakeLists.txt`.

- [ ] **Step 4: Build to verify compilation**

```bash
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug
```

- [ ] **Step 5: Commit**

```bash
git add engine/renderer/DescriptorLayoutCache.h engine/renderer/DescriptorLayoutCache.cpp engine/CMakeLists.txt
git commit -m "feat: add DescriptorLayoutCache for deduplicated layout management"
```

---

### Task 2: Enhance Shader Compiler — add members/size to reflect.json

**Files:**
- Modify: `tools/shader_compiler/main.cpp` (reflectSpirv function, lines 29-121)

- [ ] **Step 1: Enhance reflectSpirv() to extract UBO member info**

In `tools/shader_compiler/main.cpp`, modify the `reflectStage()` lambda (lines 45-84) and the JSON building section (lines 90-106) to:

1. Add `size` field to each binding (from `SpvReflectDescriptorBinding::block.size`)
2. Add `members` array to UBO bindings (from `SpvReflectDescriptorBinding::block.members`)
3. Add `members` array to push constants (from `SpvReflectBlockVariable::members`)

The `BindingInfo` struct needs new fields:
```cpp
struct BindingInfo {
    uint32_t binding;
    std::string type;
    std::string name;
    std::set<std::string> stages;
    uint32_t size = 0;  // NEW: UBO/buffer byte size
    json members;        // NEW: member info array
};
```

In the `reflectStage` lambda, after populating `info.name`, add:
```cpp
if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
    b->block.member_count > 0 && info.members.empty()) {
    info.size = b->block.size;
    info.members = json::array();
    for (uint32_t m = 0; m < b->block.member_count; m++) {
        auto& member = b->block.members[m];
        json mj;
        mj["name"] = member.name ? member.name : "";
        mj["offset"] = member.offset;
        mj["size"] = member.size;
        // Derive type string from member.type_description
        mj["type"] = descriptorMemberTypeStr(member);
        info.members.push_back(mj);
    }
}
```

Add a helper function to convert SpvReflectBlockVariable to type string:
```cpp
static std::string descriptorMemberTypeStr(const SpvReflectBlockVariable& member) {
    if (!member.type_description) return "unknown";
    auto flags = member.type_description->type_flags;
    uint32_t cols = member.type_description->traits.numeric.matrix.column_count;
    uint32_t rows = member.type_description->traits.numeric.vector.component_count;
    if (cols > 1) return "float" + std::to_string(cols) + "x" + std::to_string(rows);
    if (flags & SPV_REFLECT_TYPE_FLAG_INT) {
        if (rows > 1) return "int" + std::to_string(rows);
        return "int";
    }
    if (flags & SPV_REFLECT_TYPE_FLAG_FLOAT) {
        if (rows > 1) return "float" + std::to_string(rows);
        return "float";
    }
    return "unknown";
}
```

Similarly for push constants, extract member info from `SpvReflectBlockVariable::members`.

In the JSON building section, add `size` and `members` fields to binding JSON:
```cpp
if (info.size > 0) b["size"] = info.size;
if (!info.members.empty()) b["members"] = info.members;
```

For push constants, also add members:
```cpp
// In the pcList building, store members from SpvReflectBlockVariable
```

- [ ] **Step 2: Build shader compiler and test**

```bash
cd build3 && cmake --build . --config Debug --target ShaderCompiler
build3/tools/shader_compiler/Debug/ShaderCompiler.exe assets/shaders assets/shaders
```

Verify the generated `.reflect.json` files now contain `size` and `members` fields.

- [ ] **Step 3: Commit**

```bash
git add tools/shader_compiler/main.cpp
git commit -m "feat: shader compiler emits UBO member info in reflect.json"
```

---

### Task 3: Modify Slang shaders — move material params to set 1 ConstantBuffer

**Files:**
- Modify: `assets/shaders/Lit.slang`
- Modify: `assets/shaders/Triangle.slang`
- Modify: `assets/shaders/Unlit.slang`
- Modify: `assets/shaders/Grid.slang`

- [ ] **Step 1: Refactor Lit.slang**

Replace current UBO/PushConstants with frequency-based grouping:

```slang
// Set 0: per-frame (engine-managed)
struct FrameData {
    float4x4 view;
    float4x4 proj;
    float3 lightDir;
    float3 lightColor;
    float3 ambientColor;
    float3 cameraPos;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;

// Set 1: per-material (shader-specific)
struct LitMaterialParams {
    float4 baseColor;
    float metallic;
    float roughness;
};
[[vk::binding(0, 1)]] ConstantBuffer<LitMaterialParams> materialParams;
[[vk::binding(1, 1)]] Sampler2D albedoMap;
[[vk::binding(2, 1)]] Sampler2D normalMap;

// Push constant: per-object only
struct PushConstants {
    float4x4 model;
    int highlighted;
};
[[vk::push_constant]] PushConstants pc;
```

Update `vertexMain()`: read `materialParams.baseColor` etc. instead of `pc.baseColor`.
Update `fragmentMain()`: read `frame.lightDir` instead of `ubo.lightDir`, `materialParams` via varyings from VS.

- [ ] **Step 2: Refactor Triangle.slang**

Same pattern as Lit.slang. Use `FrameData` for set 0, `TriangleMaterialParams` for set 1 (same members as Lit — baseColor, metallic, roughness).

- [ ] **Step 3: Refactor Unlit.slang**

```slang
struct UnlitMaterialParams {
    float4 baseColor;
};
[[vk::binding(0, 1)]] ConstantBuffer<UnlitMaterialParams> materialParams;
[[vk::binding(1, 1)]] Sampler2D albedoMap;
```

Only `baseColor` in the material params UBO, no metallic/roughness.

- [ ] **Step 4: Refactor Grid.slang**

Only uses set 0. Rename `UBO` to `FrameData`, rename `ubo` variable to `frame`. No set 1. No push constants. Keep `mat4Inverse()` and `unprojectPoint()` using `frame.view`/`frame.proj`.

- [ ] **Step 5: Recompile all shaders**

```bash
build3/tools/shader_compiler/Debug/ShaderCompiler.exe assets/shaders assets/shaders
```

Verify all 4 shaders compile and `.reflect.json` files contain correct set 0 / set 1 / push constant info with members.

- [ ] **Step 6: Commit**

```bash
git add assets/shaders/
git commit -m "feat: move material params from push constants to set 1 ConstantBuffer"
```

---

### Task 4: Refactor PushConstantData and UniformBufferObject

**Files:**
- Modify: `engine/renderer/Buffer.h` (lines 55-73)

- [ ] **Step 1: Shrink PushConstantData**

```cpp
struct PushConstantData
{
    glm::mat4 model;         // 64 bytes
    int highlighted;         // 4 bytes
    int _pad[3];             // 12 bytes padding
};                           // total: 80 bytes
```

Remove `baseColor`, `metallic`, `roughness` — these move to per-material UBO in set 1.

- [ ] **Step 2: Verify no other code references removed fields directly from Buffer.h**

Search for `pc.baseColor`, `pc.metallic`, `pc.roughness` in `engine/` and `editor/`. These references will be updated in later tasks (Renderer.cpp, InspectorPanel.cpp). For now just modify the struct.

- [ ] **Step 3: Commit**

```bash
git add engine/renderer/Buffer.h
git commit -m "refactor: shrink PushConstantData to model+highlighted only"
```

---

### Task 5: Refactor Pipeline to use DescriptorLayoutCache

**Files:**
- Modify: `engine/renderer/Pipeline.h` (lines 37-77)
- Modify: `engine/renderer/Pipeline.cpp` (lines 200-393)

- [ ] **Step 1: Modify Pipeline.h**

Change `create()` to accept `DescriptorLayoutCache&`:
```cpp
void create(VkDevice device, VkRenderPass renderPass,
            VkExtent2D extent, DescriptorLayoutCache& layoutCache,
            VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
            const std::string& vertPath = "",
            const std::string& fragPath = "");
```

Remove `createWithLayouts()` method entirely.

Remove `m_reflectedLayouts` member (Pipeline no longer owns layouts — cache does).

Add include for `DescriptorLayoutCache.h`.

- [ ] **Step 2: Modify Pipeline.cpp create()**

Rewrite `Pipeline::create()` (lines 200-236):
1. Load shader binaries (unchanged)
2. Load reflection JSON (unchanged)
3. For each reflected set, build `VkDescriptorSetLayoutBinding` vector → call `layoutCache.getOrCreate()`
4. Build push constant ranges from reflection
5. Call `createPipelineCommon()` with cache-provided layouts

- [ ] **Step 3: Modify Pipeline::cleanup()**

Remove the loop that destroys `m_reflectedLayouts` (lines 387-390). Pipeline no longer owns layouts.

- [ ] **Step 4: Build to verify (will have compilation errors from callers — expected)**

The build will fail because `Renderer.cpp` and `AssetManager.cpp` still call `createWithLayouts()`. This is expected and will be fixed in Tasks 7-8.

- [ ] **Step 5: Commit**

```bash
git add engine/renderer/Pipeline.h engine/renderer/Pipeline.cpp
git commit -m "refactor: Pipeline uses DescriptorLayoutCache, remove createWithLayouts"
```

---

### Task 6: Refactor Descriptor to generic set allocator

**Files:**
- Modify: `engine/renderer/Descriptor.h`
- Modify: `engine/renderer/Descriptor.cpp`

- [ ] **Step 1: Rewrite Descriptor.h**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace QymEngine {

class Descriptor {
public:
    void createPool(VkDevice device, int maxFramesInFlight, int maxMaterials = 100);

    // Generic descriptor set allocation
    VkDescriptorSet allocateSet(VkDevice device, VkDescriptorSetLayout layout);

    // Per-frame UBO sets (set 0)
    void createPerFrameSets(VkDevice device, int maxFramesInFlight,
                            VkDescriptorSetLayout perFrameLayout,
                            const std::vector<VkBuffer>& uniformBuffers,
                            VkDeviceSize uboSize);

    VkDescriptorSet getPerFrameSet(uint32_t frame) const { return m_perFrameSets[frame]; }
    VkDescriptorPool getPool() const { return m_descriptorPool; }

    void cleanup(VkDevice device);

private:
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_perFrameSets;
};

} // namespace QymEngine
```

- [ ] **Step 2: Rewrite Descriptor.cpp**

Remove `createUboLayout()`, `createTextureLayout()`.

`createPool()` — sized for:
- `maxFramesInFlight` UNIFORM_BUFFER (per-frame UBO)
- `maxMaterials` UNIFORM_BUFFER (per-material param UBO)
- `maxMaterials * 4` COMBINED_IMAGE_SAMPLER (per-material textures)
- `maxSets = maxFramesInFlight + maxMaterials`

`allocateSet()` — generic: takes layout, allocates from pool, returns VkDescriptorSet.

`createPerFrameSets()` — allocates per-frame UBO sets using the given layout, writes descriptor updates binding the uniform buffers.

`cleanup()` — only destroys pool (no layouts).

- [ ] **Step 3: Commit**

```bash
git add engine/renderer/Descriptor.h engine/renderer/Descriptor.cpp
git commit -m "refactor: Descriptor becomes generic set allocator, no fixed layouts"
```

---

### Task 7: Refactor AssetManager — MaterialInstance + LayoutCache

**Files:**
- Modify: `engine/asset/AssetManager.h`
- Modify: `engine/asset/AssetManager.cpp`
- Delete: `engine/asset/MaterialAsset.h` (merged into AssetManager.h)

- [ ] **Step 1: Define MaterialInstance in AssetManager.h**

Replace `MaterialAsset` with:
```cpp
struct MaterialInstance {
    std::string name;
    std::string shaderPath;
    ShaderAsset* shader = nullptr;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;  // set 1
    VkBuffer paramBuffer = VK_NULL_HANDLE;
    VkDeviceMemory paramMemory = VK_NULL_HANDLE;
    void* paramMapped = nullptr;
    uint32_t paramBufferSize = 0;
    // Properties by name (from reflection members)
    std::map<std::string, glm::vec4> vec4Props;
    std::map<std::string, float> floatProps;
    std::map<std::string, std::string> texturePaths;
};
```

Add `DescriptorLayoutCache*` member and setter:
```cpp
void setLayoutCache(DescriptorLayoutCache* cache) { m_layoutCache = cache; }
```

Remove `setDescriptorSetLayouts()` (no longer needed — layouts come from cache).

Change `loadMaterial()` return type from `const MaterialAsset*` to `const MaterialInstance*`.

- [ ] **Step 2: Rewrite loadShader()**

`loadShader()` now calls `pipeline.create(device, renderPass, extent, *m_layoutCache)` instead of `createWithLayouts()`. No explicit layout passing needed.

- [ ] **Step 3: Rewrite loadMaterial()**

New flow:
1. Parse .mat.json (name, shader, properties)
2. Load shader via `loadShader()`
3. Get set 1 layout from shader's reflection → `m_layoutCache->getOrCreate()`
4. Allocate descriptor set from pool via `m_descriptor->allocateSet()`
5. Create MaterialParams UBO (size from reflection `sets[1].bindings[0].size`)
6. Load textures, write descriptor set (UBO at binding 0, samplers at binding 1+)
7. Write property values to paramBuffer at reflected offsets
8. Cache and return MaterialInstance

- [ ] **Step 4: Add MaterialInstance cleanup in shutdown()**

Destroy all `paramBuffer` / `paramMemory` in `m_materialCache`.

- [ ] **Step 5: Commit**

```bash
git add engine/asset/AssetManager.h engine/asset/AssetManager.cpp
git commit -m "feat: MaterialInstance with per-material UBO and reflection-driven descriptor sets"
```

---

### Task 8: Refactor Renderer — new init/draw flow

**Files:**
- Modify: `engine/renderer/Renderer.h`
- Modify: `engine/renderer/Renderer.cpp`

- [ ] **Step 1: Add DescriptorLayoutCache to Renderer.h**

Add member: `DescriptorLayoutCache m_layoutCache;`

Add include: `#include "renderer/DescriptorLayoutCache.h"`

Remove `m_defaultTextureSet`, `m_defaultMaterialTexSet` — replaced by MaterialInstance's descriptor set with fallback material.

Add: `MaterialInstance* m_defaultMaterial = nullptr;` for nodes without material.

- [ ] **Step 2: Rewrite Renderer::init()**

New order:
1. Init VulkanContext, SwapChain, CommandManager, RenderPass
2. Register per-frame (set 0) layout in cache:
   ```cpp
   VkDescriptorSetLayoutBinding uboBinding{};
   uboBinding.binding = 0;
   uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   uboBinding.descriptorCount = 1;
   uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
   m_perFrameLayout = m_layoutCache.getOrCreate(device, {uboBinding});
   ```
3. Create main pipeline: `m_pipeline.create(device, renderPass, extent, m_layoutCache)`
4. Create texture, mesh library, uniform buffers
5. Create descriptor pool: `m_descriptor.createPool(device, MAX_FRAMES_IN_FLIGHT, 100)`
6. Create per-frame UBO sets: `m_descriptor.createPerFrameSets(device, ..., m_perFrameLayout, ...)`
7. Create fallback textures (white + normal)
8. Init AssetManager with cache: `m_assetManager.setLayoutCache(&m_layoutCache)`
9. Scan assets

- [ ] **Step 3: Rewrite createOffscreen()**

Pipeline creation:
```cpp
m_offscreenPipeline.create(device, m_offscreenRenderPass, {width, height}, m_layoutCache);
m_wireframePipeline.create(device, m_offscreenRenderPass, {width, height}, m_layoutCache,
                           VK_POLYGON_MODE_LINE);
```

Grid pipeline: Use `m_layoutCache.getOrCreate()` for its UBO-only layout instead of creating a separate layout.

Pass cache to AssetManager:
```cpp
m_assetManager.setLayoutCache(&m_layoutCache);
m_assetManager.setOffscreenRenderPass(m_offscreenRenderPass);
m_assetManager.setOffscreenExtent({width, height});
```

Create default material instance (using Lit shader with white fallback textures).

- [ ] **Step 4: Rewrite drawSceneToOffscreen()**

New rendering loop:
```cpp
// Bind per-frame set 0 ONCE
VkDescriptorSet frameSet = m_descriptor.getPerFrameSet(m_currentFrame);

// Draw grid (set 0 only)
vkCmdBindPipeline(cmd, ..., m_gridPipeline);
vkCmdBindDescriptorSets(cmd, ..., m_gridPipelineLayout, 0, 1, &frameSet, ...);
vkCmdDraw(cmd, 6, 1, 0, 0);

// Draw scene nodes
scene.traverseNodes([&](Node* node) {
    const MaterialInstance* mat = !node->materialPath.empty()
        ? m_assetManager.loadMaterial(node->materialPath)
        : m_defaultMaterial;

    VkPipeline pipeline = mat->shader->pipeline.getPipeline();
    VkPipelineLayout layout = mat->shader->pipeline.getPipelineLayout();
    vkCmdBindPipeline(cmd, ..., pipeline);

    // Bind set 0 (per-frame) — rebind after pipeline change
    vkCmdBindDescriptorSets(cmd, ..., layout, 0, 1, &frameSet, ...);

    // Bind set 1 (per-material)
    vkCmdBindDescriptorSets(cmd, ..., layout, 1, 1, &mat->descriptorSet, ...);

    // Push constants (model + highlighted only, 80 bytes)
    PushConstantData pc{};
    pc.model = glm::transpose(node->getWorldMatrix());
    pc.highlighted = 0;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(PushConstantData), &pc);

    // Draw mesh...
});
```

Wireframe pass: same as before but bind default material's descriptor set for set 1.

Light gizmo pass: use default material.

- [ ] **Step 5: Update updateUniformBuffer()**

No structural changes needed — same UBO data, just ensure `memcpy` size matches.

- [ ] **Step 6: Update shutdown()**

Add `m_layoutCache.cleanup(device)` after all pipelines/descriptors are destroyed.

- [ ] **Step 7: Build and run editor**

```bash
taskkill /F /IM QymEditor.exe 2>NUL
cd build3 && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . --config Debug
build3/editor/Debug/QymEditor.exe
```

Verify: Scene renders correctly with grid, objects, lighting, wireframe selection.

- [ ] **Step 8: Commit**

```bash
git add engine/renderer/Renderer.h engine/renderer/Renderer.cpp
git commit -m "feat: Renderer uses DescriptorLayoutCache and MaterialInstance for rendering"
```

---

### Task 9: Editor Material Editing UI

**Files:**
- Modify: `editor/panels/InspectorPanel.cpp` (lines 53-134)

- [ ] **Step 1: Add shader switching dropdown**

When a node is selected and has a material, show a dropdown listing all `.shader.json` files from `AssetManager::getShaderFiles()`. When user selects a different shader, rebuild the MaterialInstance with the new shader (new param UBO, new descriptor set, new default properties).

- [ ] **Step 2: Dynamic property editor from reflection**

Replace hardcoded property UI with reflection-driven generation:
```cpp
// Get shader's reflected set 1 members
auto& reflection = mat->shader->pipeline.getReflection();
auto it = reflection.sets.find(1);
if (it != reflection.sets.end()) {
    for (auto& binding : it->second) {
        if (binding.type == "uniformBuffer") {
            // For each member: generate appropriate ImGui widget
            // float4 with "color" in name → ColorEdit4
            // float → DragFloat
        } else if (binding.type == "combinedImageSampler") {
            // Show texture preview + browse button
        }
    }
}
```

Property changes write directly to `mat->paramMapped` at the reflected offset.

- [ ] **Step 3: New Material / Save Material buttons**

"New Material": Create a new MaterialInstance based on the currently selected shader with default values. Assign to current node.

"Save Material": Serialize current MaterialInstance to `.mat.json`:
```json
{
  "name": "...",
  "shader": "shaders/standard_lit.shader.json",
  "properties": {
    "baseColor": [1.0, 0.2, 0.2, 1.0],
    "metallic": 0.8,
    "albedoMap": "textures/metal.jpg"
  }
}
```

- [ ] **Step 4: Build and test in editor**

Run editor, select a node, verify:
- Material properties display correctly
- Editing baseColor updates rendering in real-time
- Switching shader rebuilds the material
- Save/Load material works

- [ ] **Step 5: Commit**

```bash
git add editor/panels/InspectorPanel.cpp
git commit -m "feat: dynamic material editing UI driven by shader reflection"
```

---

### Task 10: Validation and Polish

**Files:**
- Modify: `engine/renderer/Pipeline.cpp` (set 0 validation in create())
- Modify: `engine/renderer/Renderer.cpp` (grid pipeline cleanup)

- [ ] **Step 1: Add set 0 validation in Pipeline::create()**

After loading reflection, verify set 0 bindings match the engine's fixed per-frame layout:
```cpp
// Compare reflected set 0 with the registered per-frame layout
auto it = m_reflection.sets.find(0);
if (it != m_reflection.sets.end()) {
    // Verify: exactly 1 binding, type=uniformBuffer, binding=0
    // Log error if mismatch
}
```

- [ ] **Step 2: Clean up grid pipeline to use LayoutCache**

Replace the manual grid descriptor set layout creation in `createOffscreen()` with:
```cpp
m_gridPipelineLayout = // use m_layoutCache.getOrCreate() for UBO-only layout
```

Remove manual `vkDestroyDescriptorSetLayout` for grid layout (cache owns it).

- [ ] **Step 3: Full integration test**

Run editor with the default scene:
- Verify all objects render with correct lighting/materials
- Verify grid displays correctly
- Verify wireframe selection works
- Verify light gizmos display
- Verify material editing works in Inspector
- Verify F12 RenderDoc capture works
- Capture a frame and verify correct descriptor set bindings

- [ ] **Step 4: Commit**

```bash
git add engine/renderer/Pipeline.cpp engine/renderer/Renderer.cpp
git commit -m "feat: set 0 validation and grid pipeline uses LayoutCache"
```
