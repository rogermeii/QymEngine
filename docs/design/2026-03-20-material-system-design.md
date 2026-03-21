# Material System Design

## 1. Architecture

```
Shader Asset (.shader.json)
  -> vert/frag shader paths
  -> exposed properties (name, type, default)

Material Asset (.mat.json)
  -> references Shader
  -> provides property values

Node
  -> materialPath references Material
```

## 2. Shader Asset Format

`assets/shaders/standard_lit.shader.json`:
```json
{
  "name": "Standard Lit",
  "vert": "shaders/lit.vert.spv",
  "frag": "shaders/lit.frag.spv",
  "properties": [
    { "name": "baseColor",  "type": "color4",    "default": [1,1,1,1] },
    { "name": "metallic",   "type": "float",     "default": 0.0, "range": [0,1] },
    { "name": "roughness",  "type": "float",     "default": 0.5, "range": [0,1] },
    { "name": "albedoMap",  "type": "texture2D", "default": "white" },
    { "name": "normalMap",  "type": "texture2D", "default": "normal" }
  ]
}
```

`assets/shaders/unlit.shader.json`:
```json
{
  "name": "Unlit",
  "vert": "shaders/unlit.vert.spv",
  "frag": "shaders/unlit.frag.spv",
  "properties": [
    { "name": "baseColor",  "type": "color4",    "default": [1,1,1,1] },
    { "name": "albedoMap",  "type": "texture2D", "default": "white" }
  ]
}
```

Property types: `float`, `color4`, `texture2D`.

## 3. Material Asset Format

`assets/materials/brick.mat.json`:
```json
{
  "name": "BrickWall",
  "shader": "shaders/standard_lit.shader.json",
  "properties": {
    "baseColor": [0.8, 0.3, 0.2, 1.0],
    "roughness": 0.7,
    "albedoMap": "textures/brick_albedo.jpg"
  }
}
```

Unspecified properties use Shader defaults.

## 4. Engine Data Structures

```cpp
struct ShaderProperty {
    std::string name;
    std::string type;       // "float", "color4", "texture2D"
    glm::vec4 defaultVec;   // color4 default
    float defaultFloat;     // float default
    float rangeMin, rangeMax;
    std::string defaultTex; // "white" / "normal"
};

struct ShaderAsset {
    std::string name;
    std::string vertPath, fragPath;
    std::vector<ShaderProperty> properties;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
};

struct MaterialAsset {
    std::string name;
    std::string shaderPath;
    ShaderAsset* shader = nullptr;
    glm::vec4 baseColor = {1,1,1,1};
    float metallic = 0.0f;
    float roughness = 0.5f;
    std::string albedoMapPath;
    std::string normalMapPath;
    VkDescriptorSet textureSet = VK_NULL_HANDLE; // set 1: albedo + normal
};
```

Superset layout: MaterialAsset fields are fixed. Shader properties declaration drives Inspector UI generation and default value population.

## 5. Pipeline / Descriptor Layout

Unchanged:
- Push constants: 96 bytes (model + baseColor + metallic + roughness + highlighted + pad)
- set 0: UBO (view/proj/light)

Changed:
- set 1: expanded from 1 to 2 bindings
  - binding 0: albedo map (combined image sampler)
  - binding 1: normal map (combined image sampler)
- Fallback textures: white 1x1 (albedo), (0.5, 0.5, 1.0) 1x1 (normal)
- Each ShaderAsset owns its own VkPipeline (different vert/frag), shared PipelineLayout

## 6. Shader Changes

### Lit.vert (based on existing Triangle.vert)
- Add tangent/bitangent computation for TBN matrix

### Lit.frag (based on existing Triangle.frag)
- Sample normal map from binding 1
- TBN matrix transform normal to world space
- albedo = texture(albedoMap, uv) * baseColor

### Unlit.vert
- Simplified: only pass position, texCoord, baseColor

### Unlit.frag
- `outColor = texture(albedoMap, uv) * baseColor`, no lighting

## 7. Node Changes

```cpp
class Node {
    std::string materialPath; // ref .mat.json, empty = default
    // removed: Material material;
    // removed: std::string texturePath;
};
```

## 8. Renderer Changes

In `drawSceneToOffscreen()`:
1. Load node's MaterialAsset -> get ShaderAsset
2. Bind ShaderAsset's pipeline
3. Bind MaterialAsset's textureSet (set 1)
4. Set push constants (baseColor, metallic, roughness)
5. Draw

## 9. AssetManager Extensions

- `loadShader(path)` - parse .shader.json, create pipeline
- `loadMaterial(path)` - parse .mat.json, load referenced shader and textures
- `scanAssets()` collects .shader.json and .mat.json files
- Shader and Material caching

## 10. Inspector UI

When node selected:
- Material dropdown: all .mat.json + "Default"
- Expand to show material properties, dynamically generated from Shader's properties:
  - color4 -> ImGui::ColorEdit4
  - float + range -> ImGui::SliderFloat
  - texture2D -> texture path dropdown + thumbnail preview
- Save button writes back to .mat.json
- Project panel click on .mat.json also enables editing

## 11. Default Material

Nodes without materialPath use hardcoded default: Standard Lit, white, metallic=0, roughness=0.5, no textures.

## 12. Migration Strategy

Scene deserialization backward compatibility: if JSON contains old `material` and `texturePath` fields, auto-create corresponding .mat.json and set materialPath.
