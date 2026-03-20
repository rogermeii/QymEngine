# QymEngine Milestone C Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade from 2D to 3D rendering with depth buffer, basic geometry primitives (Cube/Plane/Sphere/Quad), and orbit camera control

**Architecture:** Vertex format upgraded to vec3+normal, depth buffer added to offscreen pass. MeshLibrary replaces Buffer's VBO/IBO with per-MeshType GPU buffers. Camera class provides orbit/pan/zoom with mouse input in SceneViewPanel. Node gains meshType field for geometry selection.

**Tech Stack:** C++17, Vulkan 1.3, CMake, GLFW, GLM, ImGui (docking), nlohmann/json

**Spec:** `docs/design/2026-03-20-milestone-c-design.md`

---

## Task 0: 3D Vertex and Depth Buffer

**Goal:** Upgrade vertex format to 3D, add depth buffer to offscreen rendering

**Files:**
- Modify: `engine/renderer/Buffer.h` — Vertex struct: vec2→vec3 pos, add vec3 normal, 4 attributes
- Modify: `engine/renderer/Buffer.cpp` — update s_vertices to vec3, s_indices to uint32_t
- Modify: `engine/renderer/Pipeline.cpp` — add VkPipelineDepthStencilStateCreateInfo
- Modify: `engine/renderer/Renderer.h` — add depth image members
- Modify: `engine/renderer/Renderer.cpp` — create depth attachment, update offscreen RenderPass/Framebuffer, update createOffscreen/resizeOffscreen/destroyOffscreen
- Modify: `assets/shaders/Triangle.vert` — in vec3 inPosition, in vec3 inNormal
- Modify: `assets/shaders/Triangle.frag` — add in vec3 fragNormal (unused for now)

- [ ] **Step 0.1: Upgrade Vertex struct**

In `engine/renderer/Buffer.h`, change Vertex:
```cpp
struct Vertex {
    glm::vec3 pos;       // was vec2
    glm::vec3 color;
    glm::vec2 texCoord;
    glm::vec3 normal;    // NEW

    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 4> getAttributeDescriptions(); // was 3
};
```

In `Buffer.cpp`, update `getBindingDescription()` (stride = sizeof(Vertex), unchanged since it uses sizeof).

Update `getAttributeDescriptions()` to return 4 attributes:
- location 0: pos, VK_FORMAT_R32G32B32_SFLOAT (was R32G32), offset = offsetof(Vertex, pos)
- location 1: color, VK_FORMAT_R32G32B32_SFLOAT, offset = offsetof(Vertex, color)
- location 2: texCoord, VK_FORMAT_R32G32_SFLOAT, offset = offsetof(Vertex, texCoord)
- location 3: normal, VK_FORMAT_R32G32B32_SFLOAT, offset = offsetof(Vertex, normal)

Update `s_vertices` to use vec3 pos (add z=0) and vec3 normal (0,0,1 for quad facing +Z):
```cpp
const std::vector<Vertex> Buffer::s_vertices = {
    {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f,  0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}
};
```

Change `s_indices` from `uint16_t` to `uint32_t`:
```cpp
static const std::vector<uint32_t> s_indices;
```

Update all `vkCmdBindIndexBuffer` calls from `VK_INDEX_TYPE_UINT16` to `VK_INDEX_TYPE_UINT32`.

- [ ] **Step 0.2: Update shaders**

`assets/shaders/Triangle.vert`:
```glsl
#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    int highlighted;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) flat out int fragHighlighted;
layout(location = 3) out vec3 fragNormal;

void main() {
    gl_Position = ubo.proj * ubo.view * pc.model * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragHighlighted = pc.highlighted;
    fragNormal = mat3(pc.model) * inNormal;
}
```

`assets/shaders/Triangle.frag`:
```glsl
#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in int fragHighlighted;
layout(location = 3) in vec3 fragNormal;

layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(texSampler, fragTexCoord);
    if (fragHighlighted == 1) {
        outColor = mix(outColor, vec4(1.0, 0.5, 0.0, 1.0), 0.3);
    }
}
```

Compile:
```bash
C:/VulkanSDK/1.4.309.0/Bin/glslc.exe assets/shaders/Triangle.vert -o assets/shaders/vert.spv
C:/VulkanSDK/1.4.309.0/Bin/glslc.exe assets/shaders/Triangle.frag -o assets/shaders/frag.spv
```

- [ ] **Step 0.3: Add depth buffer to offscreen**

In `Renderer.h`, add depth image members:
```cpp
VkImage m_offscreenDepthImage = VK_NULL_HANDLE;
VkDeviceMemory m_offscreenDepthMemory = VK_NULL_HANDLE;
VkImageView m_offscreenDepthImageView = VK_NULL_HANDLE;
```

In `Renderer.cpp`, modify `createOffscreen()`:

1. After creating color image, create depth image:
```cpp
// Depth image
VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
m_texture.createImageRaw(m_context, width, height, depthFormat,
    VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreenDepthImage, m_offscreenDepthMemory);

// Depth image view
VkImageViewCreateInfo depthViewInfo{};
depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
depthViewInfo.image = m_offscreenDepthImage;
depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
depthViewInfo.format = depthFormat;
depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
depthViewInfo.subresourceRange.baseMipLevel = 0;
depthViewInfo.subresourceRange.levelCount = 1;
depthViewInfo.subresourceRange.baseArrayLayer = 0;
depthViewInfo.subresourceRange.layerCount = 1;
vkCreateImageView(device, &depthViewInfo, nullptr, &m_offscreenDepthImageView);
```

NOTE: Texture class has a `createImage()` helper. If it's private or doesn't fit, use the static `Buffer::createBuffer` pattern or directly call vkCreateImage. Read Texture.h/cpp to find a suitable helper, or create the depth image inline using raw Vulkan calls (vkCreateImage + vkAllocateMemory + vkBindImageMemory), using `m_context.findMemoryType()`.

2. Update offscreen RenderPass to have 2 attachments (color + depth):
```cpp
// Depth attachment
VkAttachmentDescription depthAttachment{};
depthAttachment.format = VK_FORMAT_D32_SFLOAT;
depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

VkAttachmentReference depthAttachmentRef{};
depthAttachmentRef.attachment = 1;
depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

// Add to subpass:
subpass.pDepthStencilAttachment = &depthAttachmentRef;

// Attachments array: {colorAttachment, depthAttachment}
```

3. Update offscreen Framebuffer to include both image views:
```cpp
std::array<VkImageView, 2> attachments = {m_offscreenImageView, m_offscreenDepthImageView};
fbInfo.attachmentCount = 2;
fbInfo.pAttachments = attachments.data();
```

4. Update `drawSceneToOffscreen()` clear values:
```cpp
std::array<VkClearValue, 2> clearValues{};
clearValues[0].color = {{0.1f, 0.1f, 0.1f, 1.0f}};
clearValues[1].depthStencil = {1.0f, 0};
renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
renderPassInfo.pClearValues = clearValues.data();
```

5. Update `destroyOffscreen()` to destroy depth resources.
6. Update `resizeOffscreen()` to recreate depth resources alongside color.

- [ ] **Step 0.4: Enable depth test in Pipeline**

In `Pipeline.cpp` `create()`, add depth stencil state:
```cpp
VkPipelineDepthStencilStateCreateInfo depthStencil{};
depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
depthStencil.depthTestEnable = VK_TRUE;
depthStencil.depthWriteEnable = VK_TRUE;
depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
depthStencil.depthBoundsTestEnable = VK_FALSE;
depthStencil.stencilTestEnable = VK_FALSE;

// In VkGraphicsPipelineCreateInfo:
pipelineInfo.pDepthStencilState = &depthStencil;  // was nullptr
```

- [ ] **Step 0.5: Build and verify**

```bash
cmake -B build2 -G "Visual Studio 17 2022" -A x64
cmake --build build2 --config Debug
```

Run QymEditor. The existing Quad should render as before but now with depth testing enabled.

- [ ] **Step 0.6: Commit**

```bash
git add engine/renderer/ assets/shaders/
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "feat: upgrade to 3D vertex format with depth buffer"
```

---

## Task 1: MeshLibrary

**Goal:** Create MeshLibrary with Quad/Cube/Plane/Sphere, Node gains meshType

**Files:**
- Create: `engine/renderer/MeshData.h` — hardcoded geometry vertex/index data
- Create: `engine/renderer/MeshLibrary.h` — MeshType enum, MeshLibrary class
- Create: `engine/renderer/MeshLibrary.cpp` — GPU buffer management per mesh type
- Modify: `engine/scene/Node.h` — add MeshType meshType field
- Modify: `engine/scene/Scene.cpp` — serialize/deserialize meshType
- Modify: `engine/renderer/Renderer.h` — add MeshLibrary member
- Modify: `engine/renderer/Renderer.cpp` — use MeshLibrary in init/drawScene/shutdown
- Modify: `engine/renderer/Buffer.h` — remove VBO/IBO members and methods (keep UBO + static createBuffer)
- Modify: `engine/renderer/Buffer.cpp` — remove VBO/IBO code

- [ ] **Step 1.1: Create MeshData.h**

Header-only file with static functions returning geometry data. Include `engine/renderer/Buffer.h` for Vertex type.

```cpp
// engine/renderer/MeshData.h
#pragma once
#include "Buffer.h"
#include <vector>
#include <cmath>

namespace QymEngine {
namespace MeshData {

inline void getQuad(std::vector<Vertex>& verts, std::vector<uint32_t>& idx) {
    verts = {
        {{-0.5f, -0.5f, 0.0f}, {1,1,1}, {0,0}, {0,0,1}},
        {{ 0.5f, -0.5f, 0.0f}, {1,1,1}, {1,0}, {0,0,1}},
        {{ 0.5f,  0.5f, 0.0f}, {1,1,1}, {1,1}, {0,0,1}},
        {{-0.5f,  0.5f, 0.0f}, {1,1,1}, {0,1}, {0,0,1}},
    };
    idx = {0,1,2, 2,3,0};
}

inline void getCube(std::vector<Vertex>& verts, std::vector<uint32_t>& idx) {
    // 24 vertices (4 per face, unique normals), 36 indices
    verts.clear(); idx.clear();
    struct FaceData { glm::vec3 normal; glm::vec3 right; glm::vec3 up; };
    FaceData faces[] = {
        {{ 0, 0, 1}, { 1, 0, 0}, { 0, 1, 0}}, // front
        {{ 0, 0,-1}, {-1, 0, 0}, { 0, 1, 0}}, // back
        {{ 1, 0, 0}, { 0, 0,-1}, { 0, 1, 0}}, // right
        {{-1, 0, 0}, { 0, 0, 1}, { 0, 1, 0}}, // left
        {{ 0, 1, 0}, { 1, 0, 0}, { 0, 0,-1}}, // top
        {{ 0,-1, 0}, { 1, 0, 0}, { 0, 0, 1}}, // bottom
    };
    for (int f = 0; f < 6; f++) {
        auto& fd = faces[f];
        uint32_t base = static_cast<uint32_t>(verts.size());
        glm::vec3 center = fd.normal * 0.5f;
        verts.push_back({center - fd.right*0.5f - fd.up*0.5f, {1,1,1}, {0,0}, fd.normal});
        verts.push_back({center + fd.right*0.5f - fd.up*0.5f, {1,1,1}, {1,0}, fd.normal});
        verts.push_back({center + fd.right*0.5f + fd.up*0.5f, {1,1,1}, {1,1}, fd.normal});
        verts.push_back({center - fd.right*0.5f + fd.up*0.5f, {1,1,1}, {0,1}, fd.normal});
        idx.insert(idx.end(), {base,base+1,base+2, base+2,base+3,base});
    }
}

inline void getPlane(std::vector<Vertex>& verts, std::vector<uint32_t>& idx) {
    verts = {
        {{-0.5f, 0.0f, -0.5f}, {1,1,1}, {0,0}, {0,1,0}},
        {{ 0.5f, 0.0f, -0.5f}, {1,1,1}, {1,0}, {0,1,0}},
        {{ 0.5f, 0.0f,  0.5f}, {1,1,1}, {1,1}, {0,1,0}},
        {{-0.5f, 0.0f,  0.5f}, {1,1,1}, {0,1}, {0,1,0}},
    };
    idx = {0,1,2, 2,3,0};
}

inline void getSphere(std::vector<Vertex>& verts, std::vector<uint32_t>& idx, int segments = 16, int rings = 16) {
    verts.clear(); idx.clear();
    const float PI = 3.14159265358979323846f;
    for (int r = 0; r <= rings; r++) {
        float phi = PI * r / rings;
        for (int s = 0; s <= segments; s++) {
            float theta = 2.0f * PI * s / segments;
            glm::vec3 pos = {
                0.5f * sin(phi) * cos(theta),
                0.5f * cos(phi),
                0.5f * sin(phi) * sin(theta)
            };
            glm::vec3 normal = glm::normalize(pos);
            glm::vec2 uv = {(float)s / segments, (float)r / rings};
            verts.push_back({pos, {1,1,1}, uv, normal});
        }
    }
    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < segments; s++) {
            uint32_t a = r * (segments + 1) + s;
            uint32_t b = a + segments + 1;
            idx.insert(idx.end(), {a, b, a+1, a+1, b, b+1});
        }
    }
}

} // namespace MeshData
} // namespace QymEngine
```

- [ ] **Step 1.2: Create MeshLibrary**

```cpp
// engine/renderer/MeshLibrary.h
#pragma once
#include "Buffer.h"
#include <vulkan/vulkan.h>
#include <unordered_map>

namespace QymEngine {

class VulkanContext;
class CommandManager;

enum class MeshType { None = 0, Quad, Cube, Plane, Sphere };

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

} // namespace QymEngine
```

```cpp
// engine/renderer/MeshLibrary.cpp
#include "MeshLibrary.h"
#include "MeshData.h"
#include "VulkanContext.h"
#include "CommandManager.h"

namespace QymEngine {

void MeshLibrary::init(VulkanContext& ctx, CommandManager& cmdMgr) {
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;

    MeshData::getQuad(verts, idx);   uploadMesh(ctx, cmdMgr, MeshType::Quad, verts, idx);
    MeshData::getCube(verts, idx);   uploadMesh(ctx, cmdMgr, MeshType::Cube, verts, idx);
    MeshData::getPlane(verts, idx);  uploadMesh(ctx, cmdMgr, MeshType::Plane, verts, idx);
    MeshData::getSphere(verts, idx); uploadMesh(ctx, cmdMgr, MeshType::Sphere, verts, idx);
}

void MeshLibrary::shutdown(VkDevice device) {
    for (auto& [type, mesh] : m_meshes) {
        vkDestroyBuffer(device, mesh.vertexBuffer, nullptr);
        vkFreeMemory(device, mesh.vertexMemory, nullptr);
        vkDestroyBuffer(device, mesh.indexBuffer, nullptr);
        vkFreeMemory(device, mesh.indexMemory, nullptr);
    }
    m_meshes.clear();
}

void MeshLibrary::bind(VkCommandBuffer cmd, MeshType type) const {
    auto it = m_meshes.find(type);
    if (it == m_meshes.end()) return;
    VkBuffer buffers[] = {it->second.vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, it->second.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
}

uint32_t MeshLibrary::getIndexCount(MeshType type) const {
    auto it = m_meshes.find(type);
    return (it != m_meshes.end()) ? it->second.indexCount : 0;
}

void MeshLibrary::uploadMesh(VulkanContext& ctx, CommandManager& cmdMgr, MeshType type,
                             const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    MeshGPU mesh;
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    VkDevice device = ctx.getDevice();

    // Vertex buffer via staging
    VkDeviceSize vSize = sizeof(Vertex) * vertices.size();
    VkBuffer stagingV; VkDeviceMemory stagingVM;
    Buffer::createBuffer(ctx, vSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingV, stagingVM);
    void* data;
    vkMapMemory(device, stagingVM, 0, vSize, 0, &data);
    memcpy(data, vertices.data(), vSize);
    vkUnmapMemory(device, stagingVM);
    Buffer::createBuffer(ctx, vSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mesh.vertexBuffer, mesh.vertexMemory);
    // Copy via single-time command
    auto cmd = cmdMgr.beginSingleTimeCommands(device);
    VkBufferCopy copyV{0, 0, vSize};
    vkCmdCopyBuffer(cmd, stagingV, mesh.vertexBuffer, 1, &copyV);
    cmdMgr.endSingleTimeCommands(device, ctx.getGraphicsQueue(), cmd);
    vkDestroyBuffer(device, stagingV, nullptr);
    vkFreeMemory(device, stagingVM, nullptr);

    // Index buffer via staging
    VkDeviceSize iSize = sizeof(uint32_t) * indices.size();
    VkBuffer stagingI; VkDeviceMemory stagingIM;
    Buffer::createBuffer(ctx, iSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingI, stagingIM);
    vkMapMemory(device, stagingIM, 0, iSize, 0, &data);
    memcpy(data, indices.data(), iSize);
    vkUnmapMemory(device, stagingIM);
    Buffer::createBuffer(ctx, iSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mesh.indexBuffer, mesh.indexMemory);
    auto cmd2 = cmdMgr.beginSingleTimeCommands(device);
    VkBufferCopy copyI{0, 0, iSize};
    vkCmdCopyBuffer(cmd2, stagingI, mesh.indexBuffer, 1, &copyI);
    cmdMgr.endSingleTimeCommands(device, ctx.getGraphicsQueue(), cmd2);
    vkDestroyBuffer(device, stagingI, nullptr);
    vkFreeMemory(device, stagingIM, nullptr);

    m_meshes[type] = mesh;
}

} // namespace QymEngine
```

- [ ] **Step 1.3: Add meshType to Node**

In `engine/scene/Node.h`:
```cpp
#include "renderer/MeshLibrary.h" // for MeshType enum
// ... in class Node:
MeshType meshType = MeshType::Cube;
```

NOTE: If circular include, move MeshType enum to a separate header or forward-declare.

- [ ] **Step 1.4: Update serialization**

In `engine/scene/Scene.cpp`, update `serializeNode()`:
```cpp
// Add to json:
const char* meshTypeNames[] = {"None", "Quad", "Cube", "Plane", "Sphere"};
j["meshType"] = meshTypeNames[static_cast<int>(node->meshType)];
```

Update `deserializeNode()`:
```cpp
if (j.contains("meshType")) {
    std::string mt = j["meshType"].get<std::string>();
    if (mt == "Quad")        node->meshType = MeshType::Quad;
    else if (mt == "Cube")   node->meshType = MeshType::Cube;
    else if (mt == "Plane")  node->meshType = MeshType::Plane;
    else if (mt == "Sphere") node->meshType = MeshType::Sphere;
    else                     node->meshType = MeshType::None;
}
```

- [ ] **Step 1.5: Integrate MeshLibrary into Renderer**

In `Renderer.h`, add `#include "MeshLibrary.h"` and member `MeshLibrary m_meshLibrary;`.

In `Renderer.cpp`:
- `init()`: call `m_meshLibrary.init(m_context, m_commandManager)` after command manager init
- `shutdown()`: call `m_meshLibrary.shutdown(device)` before context shutdown
- Remove `m_buffer.createVertexBuffer()` and `m_buffer.createIndexBuffer()` calls from init
- Remove VBO/IBO binding in `drawSceneToOffscreen()`, replace with per-node MeshLibrary::bind:

```cpp
scene.traverseNodes([&](Node* node) {
    if (node->meshType == MeshType::None) return;

    m_meshLibrary.bind(cmd, node->meshType);

    PushConstantData pc{};
    pc.model = node->getWorldMatrix();
    pc.highlighted = (node == scene.getSelectedNode()) ? 1 : 0;
    vkCmdPushConstants(cmd, m_offscreenPipeline.getPipelineLayout(),
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(PushConstantData), &pc);
    vkCmdDrawIndexed(cmd, m_meshLibrary.getIndexCount(node->meshType), 1, 0, 0, 0);
});
```

- [ ] **Step 1.6: Simplify Buffer class**

Remove from `Buffer.h`: `createVertexBuffer()`, `createIndexBuffer()`, `getVertexBuffer()`, `getIndexBuffer()`, `getIndexCount()`, `s_vertices`, `s_indices`, and VBO/IBO member variables.

Keep: `createUniformBuffers()`, `getUniformBuffers()`, `getUniformBuffersMapped()`, `cleanup()` (UBO only), static `createBuffer()`.

Remove corresponding code from `Buffer.cpp`. The static s_vertices/s_indices data is no longer needed (MeshData.h has the geometry).

- [ ] **Step 1.7: Update default node**

In `EditorApp.cpp`, change default node creation from `m_scene.createNode("Quad")` to create a Cube:
```cpp
auto* node = m_scene.createNode("Cube");
node->meshType = MeshType::Cube;
```

- [ ] **Step 1.8: Build and verify**

```bash
cmake --build build2 --config Debug
```

Run QymEditor. Default node should render as a 3D cube.

- [ ] **Step 1.9: Commit**

```bash
git add engine/renderer/ engine/scene/ editor/EditorApp.cpp
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "feat: add MeshLibrary with Cube/Plane/Sphere/Quad geometry"
```

---

## Task 2: Orbit Camera

**Goal:** Add orbit camera with mouse controls in Scene View

**Files:**
- Create: `engine/scene/Camera.h` — Camera class (orbit/pan/zoom)
- Modify: `engine/renderer/Renderer.h` — updateUniformBuffer accepts Camera
- Modify: `engine/renderer/Renderer.cpp` — use Camera for view/proj
- Modify: `editor/panels/SceneViewPanel.h` — add camera input handling
- Modify: `editor/panels/SceneViewPanel.cpp` — mouse input for orbit/pan/zoom
- Modify: `editor/EditorApp.h` — own Camera
- Modify: `editor/EditorApp.cpp` — pass Camera to renderer and SceneViewPanel

- [ ] **Step 2.1: Create Camera class**

```cpp
// engine/scene/Camera.h
#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace QymEngine {

class Camera {
public:
    glm::vec3 target = {0.0f, 0.0f, 0.0f};
    float distance = 5.0f;
    float yaw = -45.0f;
    float pitch = 30.0f;
    float fov = 45.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;

    glm::vec3 getPosition() const {
        float yawRad = glm::radians(yaw);
        float pitchRad = glm::radians(pitch);
        glm::vec3 offset = {
            distance * cos(pitchRad) * cos(yawRad),
            distance * sin(pitchRad),
            distance * cos(pitchRad) * sin(yawRad)
        };
        return target + offset;
    }

    glm::mat4 getViewMatrix() const {
        return glm::lookAt(getPosition(), target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::mat4 getProjMatrix(float aspect) const {
        glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
        proj[1][1] *= -1; // Vulkan Y-flip
        return proj;
    }

    void orbit(float deltaYaw, float deltaPitch) {
        yaw += deltaYaw;
        pitch = std::clamp(pitch + deltaPitch, -89.0f, 89.0f);
    }

    void pan(float deltaX, float deltaY) {
        float yawRad = glm::radians(yaw);
        float pitchRad = glm::radians(pitch);
        glm::vec3 forward = glm::normalize(target - getPosition());
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
        glm::vec3 up = glm::normalize(glm::cross(right, forward));
        target += right * deltaX * distance + up * deltaY * distance;
    }

    void zoom(float delta) {
        distance = std::clamp(distance + delta, 0.1f, 100.0f);
    }
};

} // namespace QymEngine
```

- [ ] **Step 2.2: Modify Renderer to use Camera**

In `Renderer.h`:
```cpp
#include "scene/Camera.h"
// Change:
void updateUniformBuffer(uint32_t currentImage); // OLD
void updateUniformBuffer(uint32_t currentImage, const Camera& camera); // NEW
```

In `Renderer.cpp`, `updateUniformBuffer()`:
```cpp
void Renderer::updateUniformBuffer(uint32_t currentImage, const Camera& camera) {
    UniformBufferObject ubo{};
    float aspect = (m_offscreenWidth > 0) ? m_offscreenWidth / (float)m_offscreenHeight : 1.0f;
    ubo.view = camera.getViewMatrix();
    ubo.proj = camera.getProjMatrix(aspect);
    memcpy(m_buffer.getUniformBuffersMapped()[currentImage], &ubo, sizeof(ubo));
}
```

In `beginFrame()`, the call to `updateUniformBuffer()` needs Camera. Options:
- Store Camera* in Renderer (set from EditorApp), or
- Move updateUniformBuffer call to EditorApp before drawScene

Recommended: Add `void setCamera(const Camera* camera)` to Renderer, store pointer. `beginFrame()` calls `updateUniformBuffer(m_currentFrame, *m_camera)`.

- [ ] **Step 2.3: Add camera input to SceneViewPanel**

In `SceneViewPanel.h`:
```cpp
#include "scene/Camera.h"
// Add to public:
void onImGuiRender(Renderer& renderer, Camera& camera);
// Add to private:
void handleCameraInput(Camera& camera);
```

In `SceneViewPanel.cpp`:
```cpp
void SceneViewPanel::handleCameraInput(Camera& camera) {
    if (!ImGui::IsWindowHovered()) return;

    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        camera.orbit(io.MouseDelta.x * 0.3f, -io.MouseDelta.y * 0.3f);
    }

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        camera.pan(-io.MouseDelta.x * 0.01f, io.MouseDelta.y * 0.01f);
    }

    if (io.MouseWheel != 0.0f) {
        camera.zoom(-io.MouseWheel * 0.5f);
    }
}
```

Call `handleCameraInput(camera)` inside `onImGuiRender()` before `ImGui::End()`.

- [ ] **Step 2.4: Update EditorApp**

In `EditorApp.h`:
```cpp
#include "scene/Camera.h"
// Add member:
Camera m_camera;
```

In `EditorApp.cpp`:
- `onInit()`: `m_renderer.setCamera(&m_camera);`
- `onUpdate()`: change `m_sceneViewPanel.onImGuiRender(m_renderer)` to `m_sceneViewPanel.onImGuiRender(m_renderer, m_camera)`

- [ ] **Step 2.5: Build and verify**

```bash
cmake --build build2 --config Debug
```

Run QymEditor. Right-drag rotates camera, middle-drag pans, scroll zooms.

- [ ] **Step 2.6: Commit**

```bash
git add engine/scene/Camera.h engine/renderer/ editor/
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "feat: add orbit camera with mouse controls in Scene View"
```

---

## Task 3: Panel and Serialization Updates

**Goal:** Add mesh type selection to Hierarchy/Inspector, update serialization

**Files:**
- Modify: `editor/panels/HierarchyPanel.cpp` — Add Node dropdown with type selection
- Modify: `editor/panels/InspectorPanel.cpp` — MeshType combo
- Modify: `editor/panels/HierarchyPanel.h` — include MeshLibrary.h for MeshType

- [ ] **Step 3.1: Hierarchy — Add Node dropdown**

Replace the "Add Node" button with a dropdown:

```cpp
if (ImGui::Button("Add..."))
    ImGui::OpenPopup("AddNodePopup");
if (ImGui::BeginPopup("AddNodePopup")) {
    if (ImGui::MenuItem("Empty"))  { auto* n = scene.createNode("Empty");  n->meshType = MeshType::None; }
    if (ImGui::MenuItem("Cube"))   { auto* n = scene.createNode("Cube");   n->meshType = MeshType::Cube; }
    if (ImGui::MenuItem("Plane"))  { auto* n = scene.createNode("Plane");  n->meshType = MeshType::Plane; }
    if (ImGui::MenuItem("Sphere")) { auto* n = scene.createNode("Sphere"); n->meshType = MeshType::Sphere; }
    if (ImGui::MenuItem("Quad"))   { auto* n = scene.createNode("Quad");   n->meshType = MeshType::Quad; }
    ImGui::EndPopup();
}
```

- [ ] **Step 3.2: Inspector — MeshType combo**

After the Transform section:

```cpp
if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
    const char* meshTypes[] = {"None", "Quad", "Cube", "Plane", "Sphere"};
    int current = static_cast<int>(selected->meshType);
    if (ImGui::Combo("Type", &current, meshTypes, IM_ARRAYSIZE(meshTypes)))
        selected->meshType = static_cast<MeshType>(current);
}
```

- [ ] **Step 3.3: Build and verify**

```bash
cmake --build build2 --config Debug
```

Run QymEditor. Add different geometry types via dropdown, change mesh type in Inspector, save/load scene with meshType preserved.

- [ ] **Step 3.4: Commit**

```bash
git add editor/panels/
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "feat: mesh type selection in Hierarchy and Inspector panels"
```

---

## Milestone C Completion Checklist

- [ ] 3D vertex format (vec3 pos + vec3 normal) working
- [ ] Depth buffer enabled, objects render with correct depth ordering
- [ ] Cube, Plane, Sphere, Quad geometry all render correctly
- [ ] Orbit camera: right-drag rotate, middle-drag pan, scroll zoom
- [ ] Camera only responds when mouse is over Scene View
- [ ] Add Node dropdown with type selection
- [ ] Inspector shows MeshType combo
- [ ] Save/Load preserves meshType
- [ ] No Vulkan validation errors
