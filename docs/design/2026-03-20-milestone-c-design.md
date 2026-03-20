# QymEngine Editor — Milestone C Design

**Date**: 2026-03-20
**Goal**: Upgrade to 3D rendering with basic geometry primitives and orbit camera
**Prerequisites**: Milestone B complete (scene tree, interactive editing, serialization)

---

## 1. Overview

Milestone C deliverables:

- Upgrade rendering pipeline from 2D to 3D (vec3 position, depth buffer, depth testing)
- MeshLibrary with 4 hardcoded geometry types: Quad, Cube, Plane, Sphere
- Node meshType field to select geometry type per node
- Orbit camera with mouse controls (right-drag rotate, middle-drag pan, scroll zoom)
- Camera input only active when mouse is over Scene View panel
- Updated panels and serialization to support meshType

Not included: lighting, Gizmo, model loading, ECS

## 2. Tech Stack Changes

| Item | Change |
|------|--------|
| Vertex format | vec2 pos -> vec3 pos, add vec3 normal |
| Depth buffer | New: offscreen depth attachment (VK_FORMAT_D32_SFLOAT) |
| New files | Camera.h, MeshLibrary.h/cpp, MeshData.h |

No new third-party dependencies.

## 3. Architecture

### 3.1 New/Modified Files

```
engine/
  scene/
    Camera.h              # NEW: Orbit camera (target, distance, yaw, pitch)
    Node.h                # MODIFY: add MeshType meshType field
    Scene.cpp             # MODIFY: serialize/deserialize meshType
  renderer/
    MeshLibrary.h         # NEW: manages VBO/IBO per MeshType
    MeshLibrary.cpp
    MeshData.h            # NEW: hardcoded vertex/index data for each geometry
    Buffer.h              # MODIFY: Vertex struct (vec3 pos, vec3 normal), remove VBO/IBO (moved to MeshLibrary)
    Buffer.cpp            # MODIFY: simplify to UBO-only management
    Renderer.h            # MODIFY: add MeshLibrary member, Camera parameter
    Renderer.cpp          # MODIFY: use MeshLibrary, depth buffer, camera
    Pipeline.cpp          # MODIFY: enable depth test, vertex input for vec3+normal

editor/
  panels/
    SceneViewPanel.h/cpp  # MODIFY: mouse input -> camera control
    HierarchyPanel.cpp    # MODIFY: Add Node dropdown with type selection
    InspectorPanel.cpp    # MODIFY: MeshType combo
  EditorApp.h/cpp         # MODIFY: own Camera, pass to renderer

assets/shaders/
    Triangle.vert         # MODIFY: vec3 inPosition, vec3 inNormal
    Triangle.frag         # MODIFY: (unchanged or minimal)
```

### 3.2 Module Dependencies

```
EditorApp
  |-- Camera (owned)
  |-- Scene (owned, nodes have meshType)
  |-- Renderer
        |-- MeshLibrary (owned, init/bind/shutdown)
        |-- depth buffer resources (offscreen)
```

Camera is pure data + math, no Vulkan dependency. MeshLibrary manages GPU resources.

## 4. Data Structures

### 4.1 Vertex (modified)

```cpp
struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;
    glm::vec3 normal;

    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 4> getAttributeDescriptions();
};
```

Binding: stride = sizeof(Vertex). Attributes: pos(location=0, vec3), color(location=1, vec3), texCoord(location=2, vec2), normal(location=3, vec3).

### 4.2 MeshType

```cpp
enum class MeshType { None, Quad, Cube, Plane, Sphere };
```

Added to Node: `MeshType meshType = MeshType::Cube;`

### 4.3 Camera

```cpp
// engine/scene/Camera.h
class Camera {
public:
    glm::vec3 target = {0.0f, 0.0f, 0.0f};
    float distance = 5.0f;
    float yaw = -45.0f;       // degrees
    float pitch = 30.0f;      // degrees, clamped to (-89, 89)
    float fov = 45.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;

    glm::vec3 getPosition() const;
    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjMatrix(float aspect) const;

    void orbit(float deltaYaw, float deltaPitch);
    void pan(float deltaX, float deltaY);
    void zoom(float delta);
};
```

**getPosition()**: Spherical coordinates from target + distance + yaw + pitch.

**orbit()**: Adds delta to yaw/pitch. Pitch clamped to (-89, 89) degrees.

**pan()**: Moves target along camera's local right and up vectors, scaled by distance.

**zoom()**: Adjusts distance, clamped to (0.1, 100).

### 4.4 MeshLibrary

```cpp
// engine/renderer/MeshLibrary.h
class MeshLibrary {
public:
    void init(VulkanContext& ctx, CommandManager& cmdMgr);
    void shutdown(VkDevice device);

    void bind(VkCommandBuffer cmd, MeshType type) const;
    uint32_t getIndexCount(MeshType type) const;

private:
    struct MeshGPU {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
    };

    std::unordered_map<MeshType, MeshGPU> m_meshes;

    void uploadMesh(VulkanContext& ctx, CommandManager& cmdMgr, MeshType type,
                    const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
};
```

### 4.5 MeshData (hardcoded geometry)

```cpp
// engine/renderer/MeshData.h
// Static functions returning vertex/index data:
namespace MeshData {
    void getQuad(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
    void getCube(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
    void getPlane(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
    void getSphere(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, int segments = 16, int rings = 16);
}
```

- **Quad**: 4 vertices on XY plane (z=0), 6 indices
- **Cube**: 24 vertices (4 per face, unique normals), 36 indices
- **Plane**: 4 vertices on XZ plane (y=0), 6 indices
- **Sphere**: UV sphere with configurable subdivision

All geometry centered at origin, unit size (e.g., cube from -0.5 to 0.5).

## 5. Rendering Pipeline Changes

### 5.1 Depth Buffer

Offscreen framebuffer gains a depth attachment:
- Format: `VK_FORMAT_D32_SFLOAT`
- Usage: `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`
- Create VkImage + VkDeviceMemory + VkImageView for depth
- Offscreen RenderPass updated: 2 attachments (color + depth)
- Offscreen Framebuffer created with both attachments

### 5.2 Pipeline Depth Test

```cpp
VkPipelineDepthStencilStateCreateInfo depthStencil{};
depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
depthStencil.depthTestEnable = VK_TRUE;
depthStencil.depthWriteEnable = VK_TRUE;
depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
depthStencil.depthBoundsTestEnable = VK_FALSE;
depthStencil.stencilTestEnable = VK_FALSE;
```

Currently Pipeline.cpp sets `pDepthStencilState = nullptr`. Change to pass the depth stencil state.

### 5.3 Render Loop

```
drawSceneToOffscreen(cmd, scene, camera):
    beginRenderPass(offscreen, clear color + depth)
    bindPipeline
    for each node in scene:
        if meshType == None: skip
        meshLibrary.bind(cmd, node->meshType)
        pushConstants(model = node->getWorldMatrix(), highlighted)
        drawIndexed(meshLibrary.getIndexCount(node->meshType))
    endRenderPass
```

### 5.4 Shader Changes

Vertex shader: `in vec3 inPosition` (was vec2), add `in vec3 inNormal` (unused for now, for future lighting). Fragment shader: unchanged.

### 5.5 Index Type

Current code uses `uint16_t` indices (`VK_INDEX_TYPE_UINT16`). Sphere may exceed 65535 vertices at high subdivision. Use `uint32_t` (`VK_INDEX_TYPE_UINT32`) for all geometry.

## 6. Camera Input in SceneViewPanel

```cpp
void SceneViewPanel::handleCameraInput(Camera& camera) {
    if (!ImGui::IsWindowHovered()) return;

    ImGuiIO& io = ImGui::GetIO();

    // Right mouse: orbit
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        camera.orbit(io.MouseDelta.x * 0.3f, io.MouseDelta.y * 0.3f);
    }

    // Middle mouse: pan
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        camera.pan(-io.MouseDelta.x * 0.01f, io.MouseDelta.y * 0.01f);
    }

    // Scroll: zoom
    if (io.MouseWheel != 0.0f) {
        camera.zoom(-io.MouseWheel * 0.5f);
    }
}
```

Called in `onImGuiRender()` before `ImGui::End()`.

## 7. Panel Updates

### 7.1 Hierarchy — Add Node Dropdown

Replace single "Add Node" button with:
```cpp
if (ImGui::Button("Add..."))
    ImGui::OpenPopup("AddNodePopup");
if (ImGui::BeginPopup("AddNodePopup")) {
    if (ImGui::MenuItem("Empty"))     { auto* n = scene.createNode("Empty");  n->meshType = MeshType::None; }
    if (ImGui::MenuItem("Cube"))      { auto* n = scene.createNode("Cube");   n->meshType = MeshType::Cube; }
    if (ImGui::MenuItem("Plane"))     { auto* n = scene.createNode("Plane");  n->meshType = MeshType::Plane; }
    if (ImGui::MenuItem("Sphere"))    { auto* n = scene.createNode("Sphere"); n->meshType = MeshType::Sphere; }
    if (ImGui::MenuItem("Quad"))      { auto* n = scene.createNode("Quad");   n->meshType = MeshType::Quad; }
    ImGui::EndPopup();
}
```

### 7.2 Inspector — MeshType Combo

```cpp
const char* meshTypes[] = {"None", "Quad", "Cube", "Plane", "Sphere"};
int current = static_cast<int>(selected->meshType);
if (ImGui::Combo("Mesh", &current, meshTypes, IM_ARRAYSIZE(meshTypes)))
    selected->meshType = static_cast<MeshType>(current);
```

### 7.3 Serialization

JSON adds `"meshType": "Cube"` per node. Deserialize maps string to enum.

## 8. Iteration Plan

### Step 0 — 3D Vertex and Depth Buffer
- Upgrade Vertex to vec3 pos + vec3 normal
- Add depth attachment to offscreen framebuffer
- Update offscreen RenderPass with depth attachment
- Enable depth test in Pipeline
- Update shaders (vec3 inPosition, vec3 inNormal)
- Recompile SPIR-V
- Verify: existing Quad renders correctly in 3D

### Step 1 — MeshLibrary
- Create MeshData.h with hardcoded geometry data (Quad, Cube, Plane, Sphere)
- Create MeshLibrary class (init, bind, getIndexCount, shutdown)
- Node adds meshType field (default: Cube)
- Renderer uses MeshLibrary instead of Buffer's VBO/IBO
- Buffer class simplified to UBO-only
- Verify: default node renders as Cube

### Step 2 — Orbit Camera
- Create Camera class (orbit, pan, zoom, view/proj matrices)
- Renderer::updateUniformBuffer accepts Camera
- SceneViewPanel handles mouse input for camera
- EditorApp owns Camera, passes to renderer and SceneViewPanel
- Verify: right-drag rotates, middle-drag pans, scroll zooms

### Step 3 — Panel and Serialization Updates
- Hierarchy: Add Node dropdown with type selection
- Inspector: MeshType combo
- Serialization: meshType field in JSON
- Verify: create different geometry types, save/load works

## 9. Risks

1. **Depth format support**: VK_FORMAT_D32_SFLOAT is widely supported but should query `vkGetPhysicalDeviceFormatProperties` to confirm. Fallback: VK_FORMAT_D24_UNORM_S8_UINT.
2. **Offscreen resize with depth**: Depth image must be recreated alongside color image on resize.
3. **Sphere vertex count**: 16x16 UV sphere = ~290 vertices, well within uint32 range.
4. **Camera gimbal lock**: Pitch clamped to (-89, 89) degrees avoids gimbal lock at poles.
5. **Index type change**: Switching from uint16 to uint32 requires updating `vkCmdBindIndexBuffer` calls to use `VK_INDEX_TYPE_UINT32`.
