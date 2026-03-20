# QymEngine Editor — Milestone D Design

**Date**: 2026-03-20
**Goal**: Add ImGuizmo gizmo controls and Blinn-Phong directional lighting
**Prerequisites**: Milestone C complete (3D rendering, MeshLibrary, orbit camera)

---

## 1. Overview

- ImGuizmo integration for Translate/Rotate/Scale gizmo in Scene View
- W/E/R keyboard shortcuts to switch gizmo mode
- Blinn-Phong directional lighting (ambient + diffuse + specular)
- Light parameters hardcoded in UBO (not yet editable)

## 2. ImGuizmo Integration

**Library**: ImGuizmo (single header+cpp), added to 3rd-party/imguizmo/

**Gizmo modes**: Translate (W), Rotate (E), Scale (R)

**Rendering**: In SceneViewPanel after ImGui::Image, use ImGuizmo::Manipulate with view/proj matrices and selected node's world matrix. On manipulation, decompose result back to local Transform (accounting for parent hierarchy).

**Input priority**: When ImGuizmo::IsUsing() is true, camera input is blocked.

## 3. Blinn-Phong Lighting

**UBO extension**:
```cpp
struct UniformBufferObject {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::vec3 lightDir;    // normalized, e.g. (-0.5, -1.0, -0.3)
    alignas(16) glm::vec3 lightColor;  // (1, 1, 1)
    alignas(16) glm::vec3 ambientColor; // (0.15, 0.15, 0.15)
    alignas(16) glm::vec3 cameraPos;
};
```

**Fragment shader**: Blinn-Phong with ambient + diffuse + specular (shininess=32).

**Vertex shader**: Pass fragWorldPos = (model * vec4(pos,1)).xyz to fragment shader.

## 4. Iteration Plan

- Step 0: Add ImGuizmo dependency
- Step 1: Blinn-Phong lighting (UBO, shaders, updateUniformBuffer)
- Step 2: Gizmo integration (SceneViewPanel, shortcuts, transform decomposition)
