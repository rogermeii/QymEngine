# QymEngine 里程碑 B 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在里程碑 A 的编辑器框架上实现可交互场景编辑（场景树、节点选中/创建/删除、Transform 编辑、高亮、JSON 序列化）

**Architecture:** 新增 engine/scene/ 模块（Node/Scene/Transform，纯数据不依赖 renderer）。Renderer::drawScene 改为接收 Scene& 遍历节点渲染，push constant 传递 per-node model 矩阵和高亮标记。编辑器面板（Hierarchy/Inspector）改为操作 Scene 数据。nlohmann/json 用于序列化。

**Tech Stack:** C++17, Vulkan 1.3, CMake, GLFW, GLM, ImGui (docking), nlohmann/json

**Spec:** `docs/design/2026-03-20-milestone-b-design.md`

---

## Task 0: 引入 nlohmann/json

**目标：** 添加 JSON 库依赖

**Files:**
- Create: `3rd-party/nlohmann/json.hpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 0.1: 下载 json.hpp**

从 GitHub releases 下载 nlohmann/json 单文件头到 `3rd-party/nlohmann/json.hpp`。

- [ ] **Step 0.2: 添加 CMake target**

在根 `CMakeLists.txt` 中 STB target 之后添加：

```cmake
# nlohmann/json (header-only)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_SOURCE_DIR}/3rd-party/nlohmann")
```

在 `engine/CMakeLists.txt` 中 `target_link_libraries` 添加 `nlohmann_json`：

```cmake
target_link_libraries(QymEngineLib PUBLIC Vulkan::Vulkan glfw glm stb nlohmann_json)
```

- [ ] **Step 0.3: 构建验证**

```bash
cmake -B build2 -G "Visual Studio 17 2022" -A x64
cmake --build build2 --config Debug
```

- [ ] **Step 0.4: 提交**

```bash
git add 3rd-party/nlohmann/ CMakeLists.txt engine/CMakeLists.txt
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "build: add nlohmann/json dependency"
```

---

## Task 1: 场景数据结构

**目标：** 创建 Transform、Node、Scene 模块

**Files:**
- Create: `engine/scene/Transform.h`
- Create: `engine/scene/Node.h`
- Create: `engine/scene/Node.cpp`
- Create: `engine/scene/Scene.h`
- Create: `engine/scene/Scene.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1.1: 创建 Transform.h**

```cpp
// engine/scene/Transform.h
#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace QymEngine {

struct Transform {
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::vec3 rotation = {0.0f, 0.0f, 0.0f};  // Euler angles in degrees
    glm::vec3 scale    = {1.0f, 1.0f, 1.0f};

    glm::mat4 getLocalMatrix() const {
        glm::mat4 mat(1.0f);
        mat = glm::translate(mat, position);
        mat = glm::rotate(mat, glm::radians(rotation.x), glm::vec3(1, 0, 0));
        mat = glm::rotate(mat, glm::radians(rotation.y), glm::vec3(0, 1, 0));
        mat = glm::rotate(mat, glm::radians(rotation.z), glm::vec3(0, 0, 1));
        mat = glm::scale(mat, scale);
        return mat;
    }
};

} // namespace QymEngine
```

- [ ] **Step 1.2: 创建 Node.h 和 Node.cpp**

```cpp
// engine/scene/Node.h
#pragma once
#include "Transform.h"
#include <string>
#include <vector>
#include <memory>

namespace QymEngine {

class Node {
public:
    explicit Node(const std::string& name = "Node");

    std::string name;
    Transform transform;

    Node* getParent() const { return m_parent; }
    const std::vector<std::unique_ptr<Node>>& getChildren() const { return m_children; }

    Node* addChild(const std::string& childName);
    void removeChild(Node* child);

    glm::mat4 getWorldMatrix() const;

private:
    Node* m_parent = nullptr;
    std::vector<std::unique_ptr<Node>> m_children;
};

} // namespace QymEngine
```

```cpp
// engine/scene/Node.cpp
#include "Node.h"
#include <algorithm>

namespace QymEngine {

Node::Node(const std::string& name) : name(name) {}

Node* Node::addChild(const std::string& childName) {
    auto child = std::make_unique<Node>(childName);
    child->m_parent = this;
    Node* ptr = child.get();
    m_children.push_back(std::move(child));
    return ptr;
}

void Node::removeChild(Node* child) {
    m_children.erase(
        std::remove_if(m_children.begin(), m_children.end(),
            [child](const std::unique_ptr<Node>& c) { return c.get() == child; }),
        m_children.end());
}

glm::mat4 Node::getWorldMatrix() const {
    glm::mat4 local = transform.getLocalMatrix();
    if (m_parent)
        return m_parent->getWorldMatrix() * local;
    return local;
}

} // namespace QymEngine
```

- [ ] **Step 1.3: 创建 Scene.h 和 Scene.cpp**

```cpp
// engine/scene/Scene.h
#pragma once
#include "Node.h"
#include <string>
#include <functional>

namespace QymEngine {

class Scene {
public:
    Scene();

    std::string name = "Untitled";

    Node* getRoot() const { return m_root.get(); }
    Node* getSelectedNode() const { return m_selectedNode; }
    void setSelectedNode(Node* node) { m_selectedNode = node; }

    Node* createNode(const std::string& nodeName, Node* parent = nullptr);
    void removeNode(Node* node);

    // Traverse all nodes depth-first, skip root
    void traverseNodes(const std::function<void(Node*)>& fn) const;

    void serialize(const std::string& path) const;
    void deserialize(const std::string& path);

private:
    void traverseRecursive(Node* node, const std::function<void(Node*)>& fn) const;

    std::unique_ptr<Node> m_root;
    Node* m_selectedNode = nullptr;
};

} // namespace QymEngine
```

```cpp
// engine/scene/Scene.cpp
#include "Scene.h"
#include <json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace QymEngine {

Scene::Scene() {
    m_root = std::make_unique<Node>("Root");
}

Node* Scene::createNode(const std::string& nodeName, Node* parent) {
    if (!parent) parent = m_root.get();
    return parent->addChild(nodeName);
}

void Scene::removeNode(Node* node) {
    if (!node || node == m_root.get()) return;
    if (m_selectedNode == node) m_selectedNode = nullptr;
    Node* parent = node->getParent();
    if (parent) parent->removeChild(node);
}

void Scene::traverseNodes(const std::function<void(Node*)>& fn) const {
    for (auto& child : m_root->getChildren())
        traverseRecursive(child.get(), fn);
}

void Scene::traverseRecursive(Node* node, const std::function<void(Node*)>& fn) const {
    fn(node);
    for (auto& child : node->getChildren())
        traverseRecursive(child.get(), fn);
}

// --- Serialization helpers ---

static json serializeNode(const Node* node) {
    json j;
    j["name"] = node->name;
    j["transform"]["position"] = {node->transform.position.x, node->transform.position.y, node->transform.position.z};
    j["transform"]["rotation"] = {node->transform.rotation.x, node->transform.rotation.y, node->transform.rotation.z};
    j["transform"]["scale"] = {node->transform.scale.x, node->transform.scale.y, node->transform.scale.z};
    j["children"] = json::array();
    for (auto& child : node->getChildren())
        j["children"].push_back(serializeNode(child.get()));
    return j;
}

static void deserializeNode(Node* parent, const json& j) {
    Node* node = parent->addChild(j.value("name", "Node"));
    if (j.contains("transform")) {
        auto& t = j["transform"];
        if (t.contains("position")) {
            auto& p = t["position"];
            node->transform.position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
        }
        if (t.contains("rotation")) {
            auto& r = t["rotation"];
            node->transform.rotation = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>()};
        }
        if (t.contains("scale")) {
            auto& s = t["scale"];
            node->transform.scale = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
        }
    }
    if (j.contains("children")) {
        for (auto& childJson : j["children"])
            deserializeNode(node, childJson);
    }
}

void Scene::serialize(const std::string& path) const {
    json j;
    j["scene"]["name"] = name;
    j["scene"]["nodes"] = json::array();
    for (auto& child : m_root->getChildren())
        j["scene"]["nodes"].push_back(serializeNode(child.get()));

    std::ofstream file(path);
    if (file.is_open())
        file << j.dump(2);
}

void Scene::deserialize(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    json j = json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.contains("scene")) return;

    // Clear existing scene
    m_selectedNode = nullptr;
    m_root = std::make_unique<Node>("Root");

    auto& sceneJson = j["scene"];
    name = sceneJson.value("name", "Untitled");
    if (sceneJson.contains("nodes")) {
        for (auto& nodeJson : sceneJson["nodes"])
            deserializeNode(m_root.get(), nodeJson);
    }
}

} // namespace QymEngine
```

- [ ] **Step 1.4: 更新 engine/CMakeLists.txt**

GLOB_RECURSE 扩展为包含 `scene/*.cpp`：

```cmake
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scene/*.cpp"
)
```

- [ ] **Step 1.5: 构建验证**

```bash
cmake --build build2 --config Debug
```

- [ ] **Step 1.6: 提交**

```bash
git add engine/scene/ engine/CMakeLists.txt
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "feat: add Scene/Node/Transform data structures with JSON serialization"
```

---

## Task 2: 场景渲染集成

**目标：** 改造 Renderer 支持按场景树渲染，添加 push constant 支持

**Files:**
- Modify: `engine/renderer/Pipeline.h` — create() 增加 push constant 参数
- Modify: `engine/renderer/Pipeline.cpp` — 实现 push constant range
- Modify: `engine/renderer/Buffer.h` — UBO 移除 model 矩阵，添加 PushConstantData 结构
- Modify: `engine/renderer/Renderer.h` — drawScene(Scene&) 签名
- Modify: `engine/renderer/Renderer.cpp` — 遍历场景节点渲染
- Modify: `engine/renderer/Descriptor.cpp` — UBO size 变更
- Modify: `assets/shaders/Triangle.vert` — push constant model 矩阵
- Modify: `assets/shaders/Triangle.frag` — push constant 高亮
- Modify: `editor/EditorApp.h` — 添加 Scene 成员
- Modify: `editor/EditorApp.cpp` — 传递 Scene 给 drawScene

- [ ] **Step 2.1: 修改 UBO 和添加 PushConstantData**

在 `engine/renderer/Buffer.h` 中：

```cpp
// UBO 移除 model，只保留 view + proj
struct UniformBufferObject {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};

// Push constant: per-node model matrix + highlight flag
struct PushConstantData {
    glm::mat4 model;       // 64 bytes
    int highlighted = 0;   // 4 bytes
    int _pad[3] = {};      // padding to 80 bytes
};
```

- [ ] **Step 2.2: 修改 Pipeline 支持 push constant**

`Pipeline.h` 的 `create()` 签名改为：

```cpp
void create(VkDevice device, VkRenderPass renderPass,
            VkDescriptorSetLayout descriptorSetLayout, VkExtent2D extent,
            const std::vector<VkPushConstantRange>& pushConstantRanges = {});
```

`Pipeline.cpp` 中 `pipelineLayoutInfo` 使用传入的 push constant ranges：

```cpp
pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size());
pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();
```

- [ ] **Step 2.3: 修改着色器**

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

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) flat out int fragHighlighted;

void main() {
    gl_Position = ubo.proj * ubo.view * pc.model * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragHighlighted = pc.highlighted;
}
```

`assets/shaders/Triangle.frag`:
```glsl
#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in int fragHighlighted;

layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(texSampler, fragTexCoord);
    if (fragHighlighted == 1) {
        outColor = mix(outColor, vec4(1.0, 0.5, 0.0, 1.0), 0.3);
    }
}
```

编译着色器：
```bash
cd assets/shaders
%VULKAN_SDK%/Bin/glslc.exe Triangle.vert -o vert.spv
%VULKAN_SDK%/Bin/glslc.exe Triangle.frag -o frag.spv
```

- [ ] **Step 2.4: 修改 Renderer**

`Renderer.h`:
- `void drawScene()` 改为 `void drawScene(Scene& scene)`
- `void drawSceneToOffscreen(VkCommandBuffer cmd)` 改为 `void drawSceneToOffscreen(VkCommandBuffer cmd, Scene& scene)`
- 添加 `#include "scene/Scene.h"`

`Renderer.cpp`:
- `init()` 中 Pipeline 创建时传入 push constant range：
  ```cpp
  VkPushConstantRange pushConstantRange{};
  pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pushConstantRange.offset = 0;
  pushConstantRange.size = sizeof(PushConstantData);
  m_offscreenPipeline.create(device, m_offscreenRenderPass, m_descriptor.getLayout(), extent, {pushConstantRange});
  ```
- `updateUniformBuffer()` 移除 model 矩阵，只更新 view + proj
- `drawSceneToOffscreen()` 改为遍历场景节点：
  ```cpp
  void Renderer::drawSceneToOffscreen(VkCommandBuffer cmd, Scene& scene) {
      // begin render pass, bind pipeline, set viewport/scissor, bind VBO/IBO/descriptors...

      scene.traverseNodes([&](Node* node) {
          PushConstantData pc{};
          pc.model = node->getWorldMatrix();
          pc.highlighted = (node == scene.getSelectedNode()) ? 1 : 0;
          vkCmdPushConstants(cmd, m_offscreenPipeline.getPipelineLayout(),
              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
              0, sizeof(PushConstantData), &pc);
          vkCmdDrawIndexed(cmd, m_buffer.getIndexCount(), 1, 0, 0, 0);
      });

      // end render pass
  }
  ```

- [ ] **Step 2.5: 修改 Descriptor UBO size**

`Descriptor.cpp` 中 `createSets()` 的 `bufferInfo.range` 改为 `sizeof(UniformBufferObject)`（现在是 2 个 mat4 = 128 bytes 而非之前的 3 个 = 192 bytes）。这应该已经使用 `sizeof(UniformBufferObject)` 了，确认即可。

- [ ] **Step 2.6: 修改 EditorApp**

`EditorApp.h` 添加：
```cpp
#include "scene/Scene.h"
// ...
Scene m_scene;
```

`EditorApp.cpp`:
- `onInit()` 中创建默认节点：`m_scene.createNode("Quad");`
- `onUpdate()` 中：`m_renderer.drawScene(m_scene);`

- [ ] **Step 2.7: 构建验证**

```bash
cmake --build build2 --config Debug
./build2/editor/Debug/QymEditor.exe
```

Expected: 场景中显示一个四边形（默认 "Quad" 节点），与之前渲染效果一致。

- [ ] **Step 2.8: 提交**

```bash
git add engine/renderer/ assets/shaders/ editor/EditorApp.h editor/EditorApp.cpp
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "feat: render scene nodes with push constants and highlight support"
```

---

## Task 3: Hierarchy 面板改造

**目标：** 递归显示场景树，支持选中/创建/删除节点

**Files:**
- Modify: `editor/panels/HierarchyPanel.h`
- Modify: `editor/panels/HierarchyPanel.cpp`
- Modify: `editor/EditorApp.cpp` — 传递 Scene 给面板

- [ ] **Step 3.1: 改造 HierarchyPanel**

```cpp
// editor/panels/HierarchyPanel.h
#pragma once
#include "scene/Scene.h"

namespace QymEngine {
class HierarchyPanel {
public:
    void onImGuiRender(Scene& scene);
private:
    void drawNodeTree(Node* node, Scene& scene);
};
}
```

```cpp
// editor/panels/HierarchyPanel.cpp
#include "HierarchyPanel.h"
#include <imgui.h>

namespace QymEngine {

void HierarchyPanel::onImGuiRender(Scene& scene) {
    ImGui::Begin("Hierarchy");

    if (ImGui::Button("Add Node")) {
        scene.createNode("New Node");
    }

    for (auto& child : scene.getRoot()->getChildren())
        drawNodeTree(child.get(), scene);

    // Click empty space to deselect
    if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
        scene.setSelectedNode(nullptr);

    ImGui::End();
}

void HierarchyPanel::drawNodeTree(Node* node, Scene& scene) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node == scene.getSelectedNode())
        flags |= ImGuiTreeNodeFlags_Selected;
    if (node->getChildren().empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    bool opened = ImGui::TreeNodeEx((void*)(intptr_t)node, flags, "%s", node->name.c_str());

    if (ImGui::IsItemClicked())
        scene.setSelectedNode(node);

    // Right-click context menu
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Create Empty Child"))
            node->addChild("New Node");
        if (ImGui::MenuItem("Delete"))
            scene.removeNode(node);
        ImGui::EndPopup();
    }

    if (opened && !(flags & ImGuiTreeNodeFlags_Leaf)) {
        for (auto& child : node->getChildren())
            drawNodeTree(child.get(), scene);
        ImGui::TreePop();
    }
}

} // namespace QymEngine
```

- [ ] **Step 3.2: 更新 EditorApp**

`EditorApp.cpp` 中 `m_hierarchyPanel.onImGuiRender()` 改为 `m_hierarchyPanel.onImGuiRender(m_scene)`。

- [ ] **Step 3.3: 构建验证**

Expected: Hierarchy 显示 "Quad" 节点，点击选中高亮，右键可创建子节点/删除，Add Node 按钮可添加新节点。选中节点在 Scene View 中有橙色高亮。

- [ ] **Step 3.4: 提交**

```bash
git add editor/panels/HierarchyPanel.h editor/panels/HierarchyPanel.cpp editor/EditorApp.cpp
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "feat: interactive Hierarchy panel with node create/delete/select"
```

---

## Task 4: Inspector 面板改造

**目标：** 显示并编辑选中节点的名称和 Transform

**Files:**
- Modify: `editor/panels/InspectorPanel.h`
- Modify: `editor/panels/InspectorPanel.cpp`
- Modify: `editor/EditorApp.cpp`

- [ ] **Step 4.1: 改造 InspectorPanel**

```cpp
// editor/panels/InspectorPanel.h
#pragma once
#include "scene/Scene.h"

namespace QymEngine {
class InspectorPanel {
public:
    void onImGuiRender(Scene& scene);
};
}
```

```cpp
// editor/panels/InspectorPanel.cpp
#include "InspectorPanel.h"
#include <imgui.h>
#include <cstring>

namespace QymEngine {

void InspectorPanel::onImGuiRender(Scene& scene) {
    ImGui::Begin("Inspector");

    Node* selected = scene.getSelectedNode();
    if (!selected) {
        ImGui::Text("No node selected");
        ImGui::End();
        return;
    }

    // Editable name
    char buf[256];
    std::strncpy(buf, selected->name.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (ImGui::InputText("Name", buf, sizeof(buf)))
        selected->name = buf;

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Position", &selected->transform.position.x, 0.1f);
        ImGui::DragFloat3("Rotation", &selected->transform.rotation.x, 1.0f);
        ImGui::DragFloat3("Scale",    &selected->transform.scale.x, 0.01f, 0.01f, 100.0f);
    }

    ImGui::End();
}

} // namespace QymEngine
```

- [ ] **Step 4.2: 更新 EditorApp**

`m_inspectorPanel.onImGuiRender()` 改为 `m_inspectorPanel.onImGuiRender(m_scene)`。

- [ ] **Step 4.3: 构建验证**

Expected: 选中节点后 Inspector 显示名称和 Transform，拖拽 DragFloat3 值时 Scene View 中物体位置/旋转/缩放实时变化。

- [ ] **Step 4.4: 提交**

```bash
git add editor/panels/InspectorPanel.h editor/panels/InspectorPanel.cpp editor/EditorApp.cpp
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "feat: interactive Inspector panel with Transform editing"
```

---

## Task 5: 场景序列化与 File 菜单

**目标：** 添加 File 菜单（Save/Load），启动时自动加载

**Files:**
- Modify: `editor/EditorApp.h`
- Modify: `editor/EditorApp.cpp`
- Create: `assets/scenes/` (directory)

- [ ] **Step 5.1: 添加 File 菜单到 EditorApp**

在 `EditorApp.cpp` 的 `onUpdate()` 中，ImGui beginFrame 之后、面板渲染之前，添加 MenuBar：

```cpp
if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Scene"))
            m_scene.serialize(std::string(ASSETS_DIR) + "/scenes/default.json");
        if (ImGui::MenuItem("Load Scene")) {
            m_scene.deserialize(std::string(ASSETS_DIR) + "/scenes/default.json");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit"))
            getWindow().shouldClose(); // or glfwSetWindowShouldClose
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}
```

- [ ] **Step 5.2: 启动时自动加载**

在 `EditorApp::onInit()` 中，创建默认节点之前检查 default.json：

```cpp
std::string scenePath = std::string(ASSETS_DIR) + "/scenes/default.json";
std::ifstream check(scenePath);
if (check.good()) {
    check.close();
    m_scene.deserialize(scenePath);
    Log::info("Loaded scene: " + scenePath);
} else {
    m_scene.createNode("Quad");
    Log::info("Created default scene");
}
```

- [ ] **Step 5.3: 创建 assets/scenes/ 目录**

```bash
mkdir -p assets/scenes
```

- [ ] **Step 5.4: 构建验证**

```bash
cmake --build build2 --config Debug
./build2/editor/Debug/QymEditor.exe
```

Expected: File 菜单可见，Save 保存到 `assets/scenes/default.json`，重启后自动加载恢复场景。

- [ ] **Step 5.5: 提交**

```bash
git add editor/EditorApp.h editor/EditorApp.cpp assets/scenes/
git -c user.name="myqtmacc" -c user.email="myqtmac@126.com" commit -m "feat: scene serialization with File menu (Save/Load)"
```

---

## 里程碑 B 完成检查

- [ ] CMake 构建通过（Debug）
- [ ] 场景树数据结构正确（创建/删除/父子关系）
- [ ] Hierarchy 面板递归显示，支持选中/创建/删除
- [ ] Inspector 面板编辑 Transform 实时反映到渲染
- [ ] 选中节点橙色高亮
- [ ] 多节点渲染正常（添加多个节点，各自独立 Transform）
- [ ] Save/Load 场景正常
- [ ] 重启后场景恢复
- [ ] 无 Vulkan validation layer 错误
