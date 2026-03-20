# QymEngine Editor — Milestone E Design

**Date**: 2026-03-20
**Goal**: Asset system with .obj model import, texture import, and per-node resource binding
**Prerequisites**: Milestone D complete (Gizmo, Blinn-Phong lighting)

---

## 1. Overview

- AssetManager: scan assets/ directory, load .obj models and .jpg/.png textures
- Per-node mesh and texture assignment via Inspector dropdown
- Multi-texture support with separate descriptor sets (set 0 = UBO, set 1 = per-node texture)
- Project panel: real-time directory browsing
- tinyobjloader for .obj parsing

## 2. New Dependencies

| Library | Purpose |
|---------|---------|
| tinyobjloader | .obj model loading, single header |

## 3. AssetManager

```cpp
// engine/asset/AssetManager.h
class AssetManager {
    void init(VulkanContext& ctx, CommandManager& cmdMgr);
    void shutdown(VkDevice device);
    void scanAssets(const std::string& assetsDir);

    // Mesh loading
    struct MeshAsset { VkBuffer vbo, ibo; VkDeviceMemory vboMem, iboMem; uint32_t indexCount; };
    const MeshAsset* getMesh(const std::string& relativePath);

    // Texture loading
    struct TextureAsset { VkImage image; VkDeviceMemory memory; VkImageView view; VkSampler sampler; VkDescriptorSet descriptorSet; };
    const TextureAsset* getTexture(const std::string& relativePath);

    const std::vector<std::string>& getMeshFiles() const;
    const std::vector<std::string>& getTextureFiles() const;
};
```

Lazy loading: resources loaded on first access, cached by path.

## 4. Node Extension

```cpp
std::string meshPath;     // empty = use built-in meshType
std::string texturePath;  // empty = use default texture
```

Rendering priority: meshPath > meshType for mesh, texturePath > default for texture.

## 5. Descriptor Set Refactor

Current: single descriptor set layout with UBO (binding 0) + texture sampler (binding 1).

New: two descriptor set layouts:
- Set 0: UBO (binding 0) — shared per frame
- Set 1: Texture sampler (binding 0) — per node/texture

Pipeline layout updated to use both sets. Render loop binds set 0 once, set 1 per node.

## 6. Panel Changes

**Project Panel**: real-time scan of assets/, folder navigation, file type icons.
**Inspector**: Mesh Source dropdown (Built-in / .obj files), Texture dropdown (Default / image files).
**Serialization**: add meshPath/texturePath to JSON.

## 7. Iteration Plan

- Step 0: Add tinyobjloader + test assets
- Step 1: AssetManager (scan, mesh loading, texture loading)
- Step 2: Descriptor set refactor (split UBO and texture into separate sets)
- Step 3: Renderer integration (per-node mesh/texture binding)
- Step 4: Panel updates (Project scan, Inspector dropdowns, serialization)

## 8. Test Assets

Download or create test .obj models (cube, suzanne/monkey) and textures (checkerboard, wood) and place in assets/models/ and assets/textures/.
