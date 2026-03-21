# Material System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the inline per-node Material struct with a Shader/Material asset system where Shader assets define exposed properties and pipeline config, and Material assets reference a Shader and provide parameter values.

**Architecture:** Shader assets (.shader.json) declare vert/frag paths and properties. Material assets (.mat.json) reference a shader and provide values. Nodes store a materialPath string. Renderer loads materials, binds the correct pipeline, and sets push constants + descriptor sets per-draw. Superset pipeline layout shared across all shaders.

**Tech Stack:** C++17, Vulkan 1.4, GLSL 4.50, nlohmann/json, stb_image, ImGui

**Spec:** `docs/design/2026-03-20-material-system-design.md`

---

## File Map

### New Files
- `engine/asset/ShaderAsset.h` — ShaderProperty + ShaderAsset structs
- `engine/asset/MaterialAsset.h` — MaterialAsset struct
- `assets/shaders/Lit.vert` — Lit vertex shader (from Triangle.vert + TBN)
- `assets/shaders/Lit.frag` — Lit fragment shader (from Triangle.frag + normal map)
- `assets/shaders/Unlit.vert` — Unlit vertex shader (simplified)
- `assets/shaders/Unlit.frag` — Unlit fragment shader (no lighting)
- `assets/shaders/standard_lit.shader.json` — Lit shader asset
- `assets/shaders/unlit.shader.json` — Unlit shader asset
- `assets/materials/default_lit.mat.json` — Sample lit material
- `assets/materials/default_unlit.mat.json` — Sample unlit material

### Modified Files
- `engine/renderer/Descriptor.h/.cpp` — set 1 expanded to 2 bindings (albedo + normal)
- `engine/renderer/Pipeline.h/.cpp` — accept vert/frag shader paths as parameters
- `engine/renderer/Renderer.h/.cpp` — fallback textures, shader/material pipeline management, drawSceneToOffscreen rewrite
- `engine/asset/AssetManager.h/.cpp` — loadShader(), loadMaterial(), scan .shader.json/.mat.json
- `engine/scene/Node.h` — replace Material struct + texturePath with materialPath
- `engine/scene/Scene.cpp` — serialize/deserialize materialPath, backward compat
- `editor/panels/InspectorPanel.h/.cpp` — dynamic material UI from shader properties
- `editor/panels/ModelPreview.cpp` — use material system for preview rendering
- `editor/EditorApp.cpp` — update model preview material binding
- `assets/shaders/compile.bat` — compile new shader files

---

## Task 1: Data Structures (ShaderAsset, MaterialAsset)

**Files:**
- Create: `engine/asset/ShaderAsset.h`
- Create: `engine/asset/MaterialAsset.h`
- Modify: `engine/scene/Node.h`

- [ ] **Step 1: Create ShaderAsset.h**

```cpp
// engine/asset/ShaderAsset.h
#pragma once
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace QymEngine {

struct ShaderProperty {
    std::string name;
    std::string type;           // "float", "color4", "texture2D"
    glm::vec4 defaultVec = {1,1,1,1};
    float defaultFloat = 0.0f;
    float rangeMin = 0.0f;
    float rangeMax = 1.0f;
    std::string defaultTex;     // "white" or "normal"
};

struct ShaderAsset {
    std::string name;
    std::string vertPath;       // relative to ASSETS_DIR, e.g. "shaders/lit.vert.spv"
    std::string fragPath;
    std::vector<ShaderProperty> properties;

    // Runtime Vulkan objects (created from vert/frag paths)
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Uses shared pipeline layout from Renderer
};

} // namespace QymEngine
```

- [ ] **Step 2: Create MaterialAsset.h**

```cpp
// engine/asset/MaterialAsset.h
#pragma once
#include <string>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace QymEngine {

struct ShaderAsset;

struct MaterialAsset {
    std::string name;
    std::string shaderPath;     // ref to .shader.json
    ShaderAsset* shader = nullptr;

    // Superset parameter values
    glm::vec4 baseColor = {1, 1, 1, 1};
    float metallic = 0.0f;
    float roughness = 0.5f;
    std::string albedoMapPath;
    std::string normalMapPath;

    // Runtime: descriptor set with albedo + normal bindings
    VkDescriptorSet textureSet = VK_NULL_HANDLE;
};

} // namespace QymEngine
```

- [ ] **Step 3: Update Node.h — replace material/texturePath with materialPath**

In `engine/scene/Node.h`:
- Remove `Material` struct definition
- Remove `Material material;` member
- Remove `std::string texturePath;` member
- Add `std::string materialPath;` member

- [ ] **Step 4: Build to verify compilation**

Run: `cmake --build build3 --config Debug --target QymEditor 2>&1 | tail -20`

This will fail with many errors from code still referencing `node->material` and `node->texturePath`. That's expected — we'll fix those in subsequent tasks. Just verify the new headers parse correctly.

- [ ] **Step 5: Commit**

```bash
git add engine/asset/ShaderAsset.h engine/asset/MaterialAsset.h engine/scene/Node.h
git commit -m "feat: add ShaderAsset/MaterialAsset structs, replace Node::material with materialPath"
```

---

## Task 2: Descriptor Set Expansion (2 Texture Bindings)

**Files:**
- Modify: `engine/renderer/Descriptor.h` (no changes needed)
- Modify: `engine/renderer/Descriptor.cpp` — `createTextureLayout()` adds binding 1

- [ ] **Step 1: Modify createTextureLayout() to have 2 bindings**

In `engine/renderer/Descriptor.cpp`, `createTextureLayout()` currently creates 1 binding (binding 0: combined image sampler). Change to 2 bindings:
- binding 0: albedo map (combined image sampler, fragment stage)
- binding 1: normal map (combined image sampler, fragment stage)

```cpp
void Descriptor::createTextureLayout(VkDevice device)
{
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    // binding 0: albedo map
    bindings[0].binding = 0;
    bindings[0].descriptorCount = 1;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].pImmutableSamplers = nullptr;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // binding 1: normal map
    bindings[1].binding = 1;
    bindings[1].descriptorCount = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].pImmutableSamplers = nullptr;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_textureSetLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture descriptor set layout!");
}
```

- [ ] **Step 2: Update createPool() — double the sampler count for 2 bindings**

In `createPool()`, the `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` pool size must be doubled (each material set needs 2 samplers). Change `maxTextures` to `maxTextures * 2` in the pool size for samplers.

- [ ] **Step 3: Update createTextureSet() — accept two image/sampler pairs**

Change signature to:
```cpp
VkDescriptorSet createTextureSet(VkDevice device,
                                  VkImageView albedoView, VkSampler albedoSampler,
                                  VkImageView normalView, VkSampler normalSampler);
```

Write both descriptors (binding 0 = albedo, binding 1 = normal) in a single `vkUpdateDescriptorSets` call.

Keep the old single-texture `createTextureSet` as an overload that fills both bindings with the same texture (for backward compat during migration).

- [ ] **Step 4: Build to verify**

Run: `cmake --build build3 --config Debug 2>&1 | tail -20`
Fix any compilation errors from call sites of the old `createTextureSet`.

- [ ] **Step 5: Commit**

```bash
git add engine/renderer/Descriptor.h engine/renderer/Descriptor.cpp
git commit -m "feat: expand texture descriptor set to 2 bindings (albedo + normal)"
```

---

## Task 3: Fallback Textures & Pipeline Refactor

**Files:**
- Modify: `engine/renderer/Renderer.h` — add fallback texture members
- Modify: `engine/renderer/Renderer.cpp` — create fallback textures, refactor pipeline creation
- Modify: `engine/renderer/Pipeline.h/.cpp` — accept shader paths parameter

- [ ] **Step 1: Add fallback texture members to Renderer.h**

```cpp
// In Renderer.h private section:
// Fallback textures for materials without maps
VkImage        m_whiteFallbackImage      = VK_NULL_HANDLE;
VkDeviceMemory m_whiteFallbackMemory     = VK_NULL_HANDLE;
VkImageView    m_whiteFallbackView       = VK_NULL_HANDLE;
VkSampler      m_fallbackSampler         = VK_NULL_HANDLE;
VkImage        m_normalFallbackImage     = VK_NULL_HANDLE;
VkDeviceMemory m_normalFallbackMemory    = VK_NULL_HANDLE;
VkImageView    m_normalFallbackView      = VK_NULL_HANDLE;
VkDescriptorSet m_defaultMaterialTexSet  = VK_NULL_HANDLE; // albedo=white, normal=default
```

- [ ] **Step 2: Create fallback textures in Renderer::init()**

After descriptor pool creation, before pipeline creation:
- White fallback: 1x1 RGBA pixel (255, 255, 255, 255)
- Normal fallback: 1x1 RGBA pixel (128, 128, 255, 255) — flat normal pointing up
- Create image, view, sampler for each
- Create a default material texture descriptor set binding both fallbacks
- Replace `m_defaultTextureSet` with `m_defaultMaterialTexSet`

- [ ] **Step 3: Refactor Pipeline::create() to accept shader paths**

Change `Pipeline::create()` signature to add vert/frag path parameters:

```cpp
void create(VkDevice device, VkRenderPass renderPass,
            const std::vector<VkDescriptorSetLayout>& descriptorSetLayouts,
            VkExtent2D extent,
            const std::vector<VkPushConstantRange>& pushConstantRanges = {},
            VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
            const std::string& vertPath = "",
            const std::string& fragPath = "");
```

When paths are empty, use current default (`ASSETS_DIR/shaders/vert.spv`, `frag.spv`). When paths are provided, use those instead. This preserves backward compat for grid/wireframe pipelines.

- [ ] **Step 4: Update all Pipeline::create() call sites in Renderer.cpp**

Pass the appropriate shader paths for offscreen pipeline and wireframe pipeline. Grid pipeline uses its own creation code (unchanged).

- [ ] **Step 5: Build and run to verify no visual regression**

Run: `cmake --build build3 --config Debug --target QymEditor && build3/editor/Debug/QymEditor.exe`
Verify: Editor starts, scene renders correctly, no crashes.

- [ ] **Step 6: Commit**

```bash
git add engine/renderer/Renderer.h engine/renderer/Renderer.cpp engine/renderer/Pipeline.h engine/renderer/Pipeline.cpp
git commit -m "feat: add fallback textures and refactor Pipeline to accept shader paths"
```

---

## Task 4: New Shaders (Lit + Unlit)

**Files:**
- Create: `assets/shaders/Lit.vert`
- Create: `assets/shaders/Lit.frag`
- Create: `assets/shaders/Unlit.vert`
- Create: `assets/shaders/Unlit.frag`
- Modify: `assets/shaders/compile.bat`

- [ ] **Step 1: Create Lit.vert (copy Triangle.vert, no changes needed)**

Lit vertex shader is identical to current Triangle.vert. Copy and rename. The TBN matrix for normal mapping will be computed in the fragment shader using `dFdx/dFdy` screen-space derivatives to avoid adding tangent vertex attributes.

- [ ] **Step 2: Create Lit.frag (based on Triangle.frag + normal map sampling)**

Key additions to the existing Triangle.frag:
- Add `layout(set = 1, binding = 1) uniform sampler2D normalMap;`
- Sample normal map: `vec3 mapNormal = texture(normalMap, fragTexCoord).rgb * 2.0 - 1.0;`
- Compute TBN matrix from screen-space derivatives:
  ```glsl
  vec3 Q1 = dFdx(fragWorldPos);
  vec3 Q2 = dFdy(fragWorldPos);
  vec2 st1 = dFdx(fragTexCoord);
  vec2 st2 = dFdy(fragTexCoord);
  vec3 T = normalize(Q1 * st2.t - Q2 * st1.t);
  vec3 B = normalize(cross(normal, T));
  mat3 TBN = mat3(T, B, normal);
  normal = normalize(TBN * mapNormal);
  ```
- albedo = `texture(albedoMap, fragTexCoord).rgb * fragBaseColor.rgb`

- [ ] **Step 3: Create Unlit.vert (simplified)**

Minimal vertex shader:
- Same UBO and push constants layout (superset compatibility)
- Pass through position, texCoord, baseColor only
- Still compute gl_Position = proj * view * model * position

- [ ] **Step 4: Create Unlit.frag**

```glsl
#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoMap;

layout(location = 1) in vec2 fragTexCoord;
layout(location = 5) in vec4 fragBaseColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texColor = texture(albedoMap, fragTexCoord);
    outColor = texColor * fragBaseColor;
}
```

Note: input locations must match vertex shader output locations for superset compatibility.

- [ ] **Step 5: Update compile.bat**

```bat
C:\VulkanSDK\1.4.309.0\Bin\glslc.exe Lit.vert -o lit_vert.spv
C:\VulkanSDK\1.4.309.0\Bin\glslc.exe Lit.frag -o lit_frag.spv
C:\VulkanSDK\1.4.309.0\Bin\glslc.exe Unlit.vert -o unlit_vert.spv
C:\VulkanSDK\1.4.309.0\Bin\glslc.exe Unlit.frag -o unlit_frag.spv
C:\VulkanSDK\1.4.309.0\Bin\glslc.exe Grid.vert -o grid_vert.spv
C:\VulkanSDK\1.4.309.0\Bin\glslc.exe Grid.frag -o grid_frag.spv
```

Keep the old Triangle.vert/frag compilation for backward compat during migration.

- [ ] **Step 6: Compile shaders**

Run: `cd assets/shaders && compile.bat`
Verify: All .spv files generated without errors.

- [ ] **Step 7: Commit**

```bash
git add assets/shaders/Lit.vert assets/shaders/Lit.frag assets/shaders/Unlit.vert assets/shaders/Unlit.frag assets/shaders/compile.bat assets/shaders/*.spv
git commit -m "feat: add Lit and Unlit shaders with normal map support"
```

---

## Task 5: Shader Asset Files & Loading

**Files:**
- Create: `assets/shaders/standard_lit.shader.json`
- Create: `assets/shaders/unlit.shader.json`
- Modify: `engine/asset/AssetManager.h` — add loadShader(), shader cache, shader file list
- Modify: `engine/asset/AssetManager.cpp` — implement loadShader(), scan .shader.json

- [ ] **Step 1: Create standard_lit.shader.json**

```json
{
  "name": "Standard Lit",
  "vert": "shaders/lit_vert.spv",
  "frag": "shaders/lit_frag.spv",
  "properties": [
    { "name": "baseColor",  "type": "color4",    "default": [1,1,1,1] },
    { "name": "metallic",   "type": "float",     "default": 0.0, "range": [0,1] },
    { "name": "roughness",  "type": "float",     "default": 0.5, "range": [0,1] },
    { "name": "albedoMap",  "type": "texture2D", "default": "white" },
    { "name": "normalMap",  "type": "texture2D", "default": "normal" }
  ]
}
```

- [ ] **Step 2: Create unlit.shader.json**

```json
{
  "name": "Unlit",
  "vert": "shaders/unlit_vert.spv",
  "frag": "shaders/unlit_frag.spv",
  "properties": [
    { "name": "baseColor",  "type": "color4",    "default": [1,1,1,1] },
    { "name": "albedoMap",  "type": "texture2D", "default": "white" }
  ]
}
```

- [ ] **Step 3: Add shader loading to AssetManager.h**

Add to AssetManager:
```cpp
#include "asset/ShaderAsset.h"

// In class AssetManager:
const ShaderAsset* loadShader(const std::string& relativePath);
const std::vector<std::string>& getShaderFiles() const { return m_shaderFiles; }

// For shader pipeline creation
void setOffscreenRenderPass(VkRenderPass renderPass) { m_offscreenRenderPass = renderPass; }
void setPipelineLayout(VkPipelineLayout layout) { m_pipelineLayout = layout; }
void setOffscreenExtent(VkExtent2D extent) { m_offscreenExtent = extent; }

// Private:
std::vector<std::string> m_shaderFiles;
std::unordered_map<std::string, ShaderAsset> m_shaderCache;
VkRenderPass m_offscreenRenderPass = VK_NULL_HANDLE;
VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
VkExtent2D m_offscreenExtent = {0, 0};
```

- [ ] **Step 4: Implement loadShader() in AssetManager.cpp**

Parse the .shader.json with nlohmann/json:
1. Read `name`, `vert`, `frag`
2. Parse `properties` array into ShaderProperty vector
3. Read SPIR-V files from `ASSETS_DIR + "/" + vertPath` and `ASSETS_DIR + "/" + fragPath`
4. Create VkShaderModule for each
5. Create VkPipeline using the shared pipeline layout, offscreen render pass, and extent
6. Destroy shader modules after pipeline creation
7. Cache and return

- [ ] **Step 5: Update scanAssets() to collect .shader.json files**

Add to the scan loop:
```cpp
if (ext == ".shader.json" || (ext == ".json" && relPath.find(".shader.") != std::string::npos)) {
    m_shaderFiles.push_back(relPath);
}
```

Actually, since `.shader.json` has extension `.json`, check the full filename pattern.

- [ ] **Step 6: Build to verify**

Run: `cmake --build build3 --config Debug 2>&1 | tail -20`

- [ ] **Step 7: Commit**

```bash
git add assets/shaders/*.shader.json engine/asset/AssetManager.h engine/asset/AssetManager.cpp engine/asset/ShaderAsset.h
git commit -m "feat: shader asset loading from .shader.json files"
```

---

## Task 6: Material Asset Files & Loading

**Files:**
- Create: `assets/materials/default_lit.mat.json`
- Create: `assets/materials/default_unlit.mat.json`
- Modify: `engine/asset/AssetManager.h` — add loadMaterial(), material cache
- Modify: `engine/asset/AssetManager.cpp` — implement loadMaterial(), scan .mat.json

- [ ] **Step 1: Create sample material files**

`assets/materials/default_lit.mat.json`:
```json
{
  "name": "Default Lit",
  "shader": "shaders/standard_lit.shader.json",
  "properties": {
    "baseColor": [1, 1, 1, 1],
    "metallic": 0.0,
    "roughness": 0.5
  }
}
```

`assets/materials/default_unlit.mat.json`:
```json
{
  "name": "Default Unlit",
  "shader": "shaders/unlit.shader.json",
  "properties": {
    "baseColor": [1, 0.2, 0.2, 1]
  }
}
```

- [ ] **Step 2: Add material loading to AssetManager.h**

```cpp
#include "asset/MaterialAsset.h"

// In class AssetManager:
const MaterialAsset* loadMaterial(const std::string& relativePath);
const std::vector<std::string>& getMaterialFiles() const { return m_materialFiles; }

// Private:
std::vector<std::string> m_materialFiles;
std::unordered_map<std::string, MaterialAsset> m_materialCache;
```

- [ ] **Step 3: Implement loadMaterial() in AssetManager.cpp**

1. Parse .mat.json: read `name`, `shader`, `properties`
2. Call `loadShader(shaderPath)` to get/cache the ShaderAsset
3. Fill MaterialAsset fields from JSON properties, using shader defaults for missing values
4. Load referenced textures (albedoMap, normalMap) via existing `loadTexture()`
5. Create descriptor set with 2 bindings:
   - binding 0: albedo texture view+sampler (or white fallback)
   - binding 1: normal texture view+sampler (or normal fallback)
6. Cache and return

For fallback textures: AssetManager needs access to Renderer's fallback image views and sampler. Add setter methods or pass them during init.

- [ ] **Step 4: Update scanAssets() to collect .mat.json files**

```cpp
if (relPath.find(".mat.json") != std::string::npos) {
    m_materialFiles.push_back(relPath);
}
```

- [ ] **Step 5: Build to verify**

Run: `cmake --build build3 --config Debug 2>&1 | tail -20`

- [ ] **Step 6: Commit**

```bash
git add assets/materials/*.mat.json engine/asset/AssetManager.h engine/asset/AssetManager.cpp engine/asset/MaterialAsset.h
git commit -m "feat: material asset loading from .mat.json files"
```

---

## Task 7: Scene Serialization & Renderer Integration

**Files:**
- Modify: `engine/scene/Scene.cpp` — materialPath serialization + backward compat
- Modify: `engine/renderer/Renderer.h/.cpp` — drawSceneToOffscreen uses material system
- Modify: `editor/EditorApp.cpp` — pass fallback info to AssetManager, update model preview material binding

- [ ] **Step 1: Update Scene serialization**

In `serializeNode()`:
- Remove: baseColor, metallic, roughness, texturePath serialization
- Add: `j["materialPath"] = node->materialPath;`

In `deserializeNode()`:
- Add: `if (j.contains("materialPath")) node->materialPath = j["materialPath"];`
- Backward compat: if JSON has old `material` and/or `texturePath` fields but no `materialPath`, auto-generate a .mat.json file in `assets/materials/` with those values and set `node->materialPath` to it

- [ ] **Step 2: Update Renderer::drawSceneToOffscreen()**

Replace the current per-node rendering logic:

```cpp
// For each node:
// 1. Load material (or use default)
const MaterialAsset* mat = nullptr;
if (!node->materialPath.empty())
    mat = m_assetManager.loadMaterial(node->materialPath);

// 2. Bind shader pipeline
VkPipeline pipeline = mat && mat->shader
    ? mat->shader->pipeline
    : m_offscreenPipeline.getPipeline(); // fallback to default lit
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

// 3. Bind material texture set (or default)
VkDescriptorSet texSet = (mat && mat->textureSet != VK_NULL_HANDLE)
    ? mat->textureSet
    : m_defaultMaterialTexSet;
vkCmdBindDescriptorSets(cmd, ..., 1, 1, &texSet, 0, nullptr);

// 4. Push constants from material
PushConstantData pc{};
pc.model = node->getWorldMatrix();
pc.baseColor = mat ? mat->baseColor : glm::vec4(1);
pc.metallic = mat ? mat->metallic : 0.0f;
pc.roughness = mat ? mat->roughness : 0.5f;
pc.highlighted = 0;
vkCmdPushConstants(cmd, ...);

// 5. Draw mesh (unchanged)
```

- [ ] **Step 3: Pass fallback textures to AssetManager**

In Renderer::init() or createOffscreen(), after creating fallback textures:
```cpp
m_assetManager.setFallbackAlbedo(m_whiteFallbackView, m_fallbackSampler);
m_assetManager.setFallbackNormal(m_normalFallbackView, m_fallbackSampler);
```

Add these setters to AssetManager.

- [ ] **Step 4: Update EditorApp model preview material binding**

In `EditorApp::onUpdate()`, the model preview section needs to pass material info. For now, model preview always uses default material (no material-aware preview yet — that's Task 9).

- [ ] **Step 5: Build, run, verify**

Run: `cmake --build build3 --config Debug --target QymEditor && build3/editor/Debug/QymEditor.exe`

Verify:
- Scene loads (backward compat from old JSON)
- Nodes without materialPath render with default white lit material
- No crashes

- [ ] **Step 6: Commit**

```bash
git add engine/scene/Scene.cpp engine/renderer/Renderer.h engine/renderer/Renderer.cpp engine/asset/AssetManager.h engine/asset/AssetManager.cpp editor/EditorApp.cpp
git commit -m "feat: integrate material system into renderer and scene serialization"
```

---

## Task 8: Inspector UI for Material Editing

**Files:**
- Modify: `editor/panels/InspectorPanel.h` — may need AssetManager reference
- Modify: `editor/panels/InspectorPanel.cpp` — material dropdown + dynamic property UI

- [ ] **Step 1: Replace old Material/Texture UI with material system**

Remove the old "Material" collapsing header (ColorEdit4, SliderFloat x2).
Remove the old "Texture" collapsing header (texture dropdown + preview).

Add new "Material" section:

```cpp
if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
    // Material file dropdown
    std::vector<std::string> matItems = {"Default"};
    for (auto& f : assetManager.getMaterialFiles())
        matItems.push_back(f);

    int currentMat = 0;
    for (int i = 1; i < (int)matItems.size(); i++) {
        if (matItems[i] == selected->materialPath) { currentMat = i; break; }
    }

    if (ImGui::BeginCombo("Material File", matItems[currentMat].c_str())) {
        for (int i = 0; i < (int)matItems.size(); i++) {
            bool isSelected = (currentMat == i);
            if (ImGui::Selectable(matItems[i].c_str(), isSelected)) {
                selected->materialPath = (i == 0) ? "" : matItems[i];
            }
        }
        ImGui::EndCombo();
    }

    // Show material properties if a material is assigned
    if (!selected->materialPath.empty()) {
        auto* mat = assetManager.loadMaterial(selected->materialPath);
        if (mat && mat->shader) {
            ImGui::Separator();
            ImGui::Text("Shader: %s", mat->shader->name.c_str());

            // Dynamic UI from shader properties
            bool modified = false;
            MaterialAsset* mutableMat = const_cast<MaterialAsset*>(mat);
            for (auto& prop : mat->shader->properties) {
                if (prop.type == "color4" && prop.name == "baseColor") {
                    modified |= ImGui::ColorEdit4("Base Color", &mutableMat->baseColor.x);
                } else if (prop.type == "float" && prop.name == "metallic") {
                    modified |= ImGui::SliderFloat("Metallic", &mutableMat->metallic, prop.rangeMin, prop.rangeMax);
                } else if (prop.type == "float" && prop.name == "roughness") {
                    modified |= ImGui::SliderFloat("Roughness", &mutableMat->roughness, prop.rangeMin, prop.rangeMax);
                } else if (prop.type == "texture2D") {
                    // Texture dropdown for albedoMap/normalMap
                    // ... texture file selection UI
                }
            }

            // Save button
            if (ImGui::Button("Save Material")) {
                // Write mat back to .mat.json
            }
        }
    }
}
```

- [ ] **Step 2: Implement material save function**

Write a helper that serializes MaterialAsset back to its .mat.json file using nlohmann/json.

- [ ] **Step 3: Add .mat.json preview in Project panel click**

When a .mat.json file is selected in ProjectPanel, show material properties in Inspector (similar to texture/model preview).

- [ ] **Step 4: Build, run, verify**

Verify:
- Material dropdown shows available .mat.json files
- Selecting a material shows its properties
- Editing properties affects rendering in real-time
- Save button writes changes back to file

- [ ] **Step 5: Commit**

```bash
git add editor/panels/InspectorPanel.h editor/panels/InspectorPanel.cpp
git commit -m "feat: Inspector material editing UI with dynamic shader properties"
```

---

## Task 9: Model Preview & Polish

**Files:**
- Modify: `editor/panels/ModelPreview.cpp` — use material texture sets
- Modify: `editor/EditorApp.cpp` — model preview with material awareness
- Modify: `editor/panels/ProjectPanel.h/.cpp` — detect .mat.json files

- [ ] **Step 1: Update ModelPreview to accept material descriptor set**

In ModelPreview::renderBuiltIn() and renderMesh(), change the texture set binding to accept an optional material texture set parameter. If nullptr, use the default fallback set.

- [ ] **Step 2: Update EditorApp model preview section**

When a node is selected, look up its material and pass the material's texture set to ModelPreview. When a model file is selected in Project panel, use default material.

- [ ] **Step 3: Add .mat.json detection to ProjectPanel**

Add `isSelectedMaterial()` method to ProjectPanel:
```cpp
bool isSelectedMaterial() const {
    return m_selectedFile.find(".mat.json") != std::string::npos;
}
```

- [ ] **Step 4: Shader cleanup — remove old Triangle.vert/frag references**

Once all rendering uses the new Lit/Unlit shaders:
- Update compile.bat to remove old Triangle.vert/frag compilation (keep files for reference)
- Update Renderer::createOffscreen() to use lit shader as default offscreen pipeline

- [ ] **Step 5: Create a test scene with multiple materials**

Update `assets/scenes/default.json` to have nodes with different materialPaths. Or create new .mat.json files with different colors/textures to verify the system works end-to-end.

- [ ] **Step 6: Full integration test**

Run editor, verify:
- Multiple nodes with different materials render correctly
- Changing material on a node updates rendering immediately
- Unlit material renders without lighting
- Normal map affects lit material shading
- Material save/load works
- Scene save/load preserves materialPath
- Backward compat: old scenes still load

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat: complete material system with preview, polish, and integration"
```

---

## Summary

| Task | Description | Key Files |
|------|-------------|-----------|
| 1 | Data structures | ShaderAsset.h, MaterialAsset.h, Node.h |
| 2 | Descriptor set expansion | Descriptor.cpp |
| 3 | Fallback textures + Pipeline refactor | Renderer.cpp, Pipeline.cpp |
| 4 | New shaders (Lit + Unlit) | Lit.vert/frag, Unlit.vert/frag |
| 5 | Shader asset loading | AssetManager.cpp, .shader.json |
| 6 | Material asset loading | AssetManager.cpp, .mat.json |
| 7 | Scene + Renderer integration | Scene.cpp, Renderer.cpp |
| 8 | Inspector UI | InspectorPanel.cpp |
| 9 | Model preview + Polish | ModelPreview.cpp, EditorApp.cpp |
