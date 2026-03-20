# QymEngine 编辑器 — 里程碑 B 设计文档

**日期**: 2026-03-20
**目标**: 在里程碑 A 的编辑器框架上实现可交互的场景编辑
**前置**: 里程碑 A 已完成（模块化引擎 + 5 面板 ImGui 编辑器 + offscreen Scene View）

---

## 1. 概述

里程碑 B 的交付目标：

- 场景树/Node 数据结构，支持父子层级
- Hierarchy 面板递归显示场景树，支持选中/创建/删除节点
- Inspector 面板编辑选中节点的 Transform（Position/Rotation/Scale）
- 渲染管线从硬编码改为根据场景树动态渲染
- 选中物体高亮显示
- 基础场景序列化（JSON 保存/加载）

不包含：Gizmo 操作手柄、多种几何体、相机/灯光节点

## 2. 技术栈变更

| 项目 | 变更 |
|------|------|
| nlohmann/json | 新增，header-only，用于场景序列化 |

## 3. 架构设计

### 3.1 新增目录结构

```
engine/scene/
├── Transform.h          # Transform 数据（position/rotation/scale + 矩阵计算）
├── Node.h               # 场景节点（名称 + Transform + 父子关系）
├── Node.cpp
├── Scene.h              # 场景管理（节点增删 + 选中 + 序列化）
└── Scene.cpp

3rd-party/
└── nlohmann/
    └── json.hpp         # JSON 库（单文件）
```

### 3.2 模块依赖

```
Editor 层
    │ 使用 Scene 来驱动面板和渲染
    ▼
Engine 层
    ├── scene/  (新增)  ← 不依赖 renderer
    ├── renderer/       ← drawScene 接收 Scene& 参数
    └── core/
    │
    ▼
第三方库（+ nlohmann/json）
```

Scene 模块属于 Engine 层，不依赖 renderer（纯数据 + 序列化），renderer 通过参数接收 Scene。

## 4. 数据结构

### 4.1 Transform

```cpp
// engine/scene/Transform.h
struct Transform {
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::vec3 rotation = {0.0f, 0.0f, 0.0f};  // Euler angles, degrees
    glm::vec3 scale    = {1.0f, 1.0f, 1.0f};

    glm::mat4 getLocalMatrix() const;  // TRS composition
};
```

`getLocalMatrix()` 组合顺序：Scale → Rotate(Z) → Rotate(Y) → Rotate(X) → Translate

### 4.2 Node

```cpp
// engine/scene/Node.h
class Node {
public:
    std::string name;
    Transform transform;

    Node* getParent() const;
    const std::vector<std::unique_ptr<Node>>& getChildren() const;
    Node* addChild(const std::string& name);
    void removeChild(Node* child);

    glm::mat4 getWorldMatrix() const;  // recursive: parent.world * local

private:
    Node* m_parent = nullptr;
    std::vector<std::unique_ptr<Node>> m_children;
};
```

### 4.3 Scene

```cpp
// engine/scene/Scene.h
class Scene {
public:
    Scene();

    std::string name = "Untitled";

    Node* getRoot() const;             // invisible root node
    Node* getSelectedNode() const;
    void setSelectedNode(Node* node);

    Node* createNode(const std::string& name, Node* parent = nullptr);
    void removeNode(Node* node);       // if removed node == selectedNode, auto clear selection

    void serialize(const std::string& path) const;
    void deserialize(const std::string& path);

private:
    std::unique_ptr<Node> m_root;
    Node* m_selectedNode = nullptr;
};
```

## 5. 编辑器面板改造

### 5.1 Hierarchy 面板

- 递归显示场景树，使用 `ImGui::TreeNodeEx`
- `ImGuiTreeNodeFlags_Selected` 标记当前选中节点
- 点击节点：`scene.setSelectedNode(node)`
- 右键菜单：
  - "Create Empty Child" — `node->addChild("New Node")`
  - "Delete" — `scene.removeNode(node)`（不可删除 root）
- 顶部 "Add Node" 按钮：在 root 下创建新节点

### 5.2 Inspector 面板

- 仅在 `selectedNode != nullptr` 时显示内容
- 名称：使用 `char buf[256]` 中转缓冲区 + `ImGui::InputText`，回车或失焦后同步回 `node->name`
- Transform 组件（`ImGui::CollapsingHeader`）：
  - `ImGui::DragFloat3("Position", &transform.position, 0.1f)`
  - `ImGui::DragFloat3("Rotation", &transform.rotation, 0.1f)`
  - `ImGui::DragFloat3("Scale", &transform.scale, 0.01f, 0.01f, 100.0f)`
- 值修改实时反映到渲染

### 5.3 Scene View

- 保持 offscreen 渲染，改为遍历场景树
- 选中节点高亮：通过 push constant 传入 `isHighlighted` 标记，片段着色器叠加高亮色

### 5.4 Project / Console

- 保持不变

## 6. 渲染管线改造

### 6.1 drawScene 改造

`Renderer::drawScene()` 改为 `Renderer::drawScene(Scene& scene)`，内部调用 `drawSceneToOffscreen(VkCommandBuffer, Scene&)`。`EditorApp::onUpdate()` 改为 `m_renderer.drawScene(m_scene)`。

**多节点 UBO 更新策略**：使用 push constant 传递 model 矩阵（`glm::mat4`，64 bytes），取代 UBO 中的 model 字段。view/proj 矩阵保留在 UBO 中（每帧更新一次）。这样每个节点只需 `vkCmdPushConstants` + `vkCmdDrawIndexed`，无需多次 memcpy UBO。

UBO 简化为：
```cpp
struct UniformBufferObject {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};
```

渲染伪代码：
```
drawSceneToOffscreen(cmd, scene):
    beginRenderPass(offscreen)
    bindPipeline, bindDescriptorSets, bindVertexBuffer, bindIndexBuffer
    for each node in scene (depth-first, skip root):
        pushConstants(cmd, model = node.getWorldMatrix(), highlighted)
        drawIndexed(sharedMesh)
    endRenderPass
```

### 6.2 Push Constant

```cpp
struct PushConstantData {
    glm::mat4 model;        // 64 bytes
    int highlighted = 0;    // 4 bytes
    // padding to 80 bytes (alignment)
};
```

Pipeline layout 添加 push constant range（`VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT`，80 bytes）。`Pipeline::create()` 增加 `std::vector<VkPushConstantRange>` 参数。

### 6.3 着色器改动

Vertex shader:
```glsl
layout(push_constant) uniform PushConstants {
    mat4 model;
    int highlighted;
} pc;

// In main(): use pc.model instead of ubo.model
gl_Position = ubo.proj * ubo.view * pc.model * vec4(inPosition, 0.0, 1.0);
```

Fragment shader:
```glsl
layout(push_constant) uniform PushConstants {
    mat4 model;
    int highlighted;
} pc;

// In main():
if (pc.highlighted == 1) {
    outColor = mix(outColor, vec4(1.0, 0.5, 0.0, 1.0), 0.3);  // orange tint
}
```

需要重新编译 GLSL -> SPIR-V。

## 7. 场景序列化

### 7.1 JSON 格式

```json
{
  "scene": {
    "name": "Default Scene",
    "nodes": [
      {
        "name": "Quad 1",
        "transform": {
          "position": [0.0, 0.0, 0.0],
          "rotation": [0.0, 0.0, 0.0],
          "scale": [1.0, 1.0, 1.0]
        },
        "children": [
          {
            "name": "Child",
            "transform": { ... },
            "children": []
          }
        ]
      }
    ]
  }
}
```

### 7.2 保存/加载

- 使用 nlohmann/json 库
- 固定路径：`assets/scenes/default.json`
- EditorApp 添加 File 菜单（ImGui MenuBar）：
  - Save Scene → `scene.serialize("assets/scenes/default.json")`
  - Load Scene → `scene.deserialize("assets/scenes/default.json")`
- 启动时：如果 `default.json` 存在则加载，否则创建包含一个默认四边形节点的空场景

## 8. 迭代计划

### Step 0 — 引入 nlohmann/json
- 下载 `json.hpp` 到 `3rd-party/nlohmann/`
- CMake 添加 interface target
- 验证：编译通过

### Step 1 — 场景数据结构
- 创建 Transform.h、Node.h/cpp、Scene.h/cpp
- 场景树基本操作：createNode / removeNode
- getWorldMatrix 递归计算
- 更新 engine/CMakeLists.txt
- 验证：编译通过

### Step 2 — 场景渲染集成
- 改造 Renderer::drawScene(Scene&)
- 添加 push constant 到管线布局
- 修改着色器添加高亮支持，重新编译 SPIR-V
- EditorApp 创建 Scene 实例，传入 drawScene
- 验证：场景中显示默认四边形节点，可旋转

### Step 3 — Hierarchy 面板改造
- 递归 TreeNode 显示场景树
- 点击选中，右键菜单（Create / Delete）
- 验证：创建/删除节点，Hierarchy 正确更新

### Step 4 — Inspector 面板改造
- 显示选中节点名称（可编辑）
- DragFloat3 编辑 Transform
- 验证：修改 Transform 值，Scene View 中物体位置/旋转/缩放实时变化

### Step 5 — 场景序列化
- 实现 Scene::serialize / deserialize（nlohmann/json）
- EditorApp 添加 File 菜单（Save / Load）
- 启动时自动加载 default.json
- 验证：保存场景 → 重启 → 场景恢复

## 9. 风险与注意事项

1. **UBO 更新频率**：每个节点需要更新一次 model 矩阵到 UBO，当前是 persistent mapped memory，每帧 memcpy 即可。节点数量少时无性能问题。
2. **push constant 兼容性**：push constant 最小保证 128 bytes，4 bytes 的 int 完全安全。
3. **场景树递归深度**：栈溢出风险极低，编辑器场景不会有极深层级。`addChild` 不检查循环引用（学习项目简化）。
4. **序列化路径**：使用 ASSETS_DIR 宏构建绝对路径（`std::string(ASSETS_DIR) + "/scenes/default.json"`），确保运行时能找到文件。`assets/scenes/` 目录在 Step 5 中创建。
5. **engine/CMakeLists.txt**：现有 `GLOB_RECURSE` 模式 `core/*.cpp` 需扩展为同时包含 `scene/*.cpp`。
