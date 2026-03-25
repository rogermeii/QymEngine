# 编辑器功能增强设计

**日期**: 2026-03-21
**目标**: 实现 Undo/Redo、多选、复制/粘贴、拖拽排序、Save As/New Scene 五个编辑器核心功能。

## 1. Undo/Redo（场景快照栈）

- 新增 `editor/UndoManager.h`
- 维护 `std::deque<std::string>` 快照栈，存储 JSON 序列化的场景状态
- 每次关键操作前保存快照：创建/删除节点、修改 Transform、修改材质/网格引用
- Ctrl+Z 回退，Ctrl+Y 重做
- 栈深度限制 50 步
- Inspector 属性修改：在拖拽开始时保存快照（非每帧）

## 2. 多选

- Scene 新增 `std::unordered_set<Node*> m_selectedNodes`
- 保留 `getSelectedNode()` 返回"主选中节点"（最后一个选中的）
- Hierarchy 中：单击=单选，Ctrl+单击=切换，Shift+单击=范围选
- Inspector 多选时显示 Transform 可批量编辑
- SceneView 多选节点都显示 wireframe 高亮

## 3. 复制/粘贴

- 新增 `editor/Clipboard.h`，存储序列化的节点 JSON
- 利用已有 `serializeNode/deserializeNode`
- Ctrl+C 复制，Ctrl+V 粘贴为同级兄弟
- Ctrl+D 快速复制（Duplicate）
- Delete 键删除选中节点（支持多选批量）

## 4. 拖拽排序

- Hierarchy 使用 ImGui DragDrop API (`BeginDragDropSource/Target`)
- 拖拽到节点上 = 成为子节点
- 拖拽到节点之间 = 调整顺序（插入线视觉反馈）
- 操作可通过 UndoManager 撤销

## 5. Save As / New Scene

- File 菜单新增：New Scene、Save As
- New Scene (Ctrl+N)：清空场景，创建默认空场景
- Save (Ctrl+S)：保存到当前路径
- Save As (Ctrl+Shift+S)：ImGui 弹窗输入文件名，保存到 assets/scenes/
- 场景跟踪当前文件路径

## 文件结构

```
新增:
  editor/UndoManager.h     — 快照栈 + undo/redo
  editor/Clipboard.h       — 节点复制/粘贴剪贴板

修改:
  engine/scene/Scene.h/cpp       — 多选 API、toJson/fromJson
  engine/scene/Node.h/cpp        — clone 方法
  editor/EditorApp.h/cpp         — 快捷键、菜单扩展、UndoManager/Clipboard 集成
  editor/panels/HierarchyPanel.h/cpp  — 多选、拖拽、Delete 键
  editor/panels/InspectorPanel.cpp    — 多选批量编辑、undo 触发点
```

## 实现顺序

1. Undo/Redo（基础设施）
2. 多选（选择系统升级）
3. 复制/粘贴（依赖多选 + 序列化）
4. 拖拽排序（Hierarchy 交互）
5. Save As / New Scene（文件操作）
