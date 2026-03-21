#include "HierarchyPanel.h"
#include "UndoManager.h"
#include "Clipboard.h"
#include <imgui.h>

namespace QymEngine {

void HierarchyPanel::onImGuiRender(Scene& scene, UndoManager* undo, Clipboard* clipboard) {
    ImGui::Begin("Hierarchy");

    if (ImGui::Button("Add..."))
        ImGui::OpenPopup("AddNodePopup");
    if (ImGui::BeginPopup("AddNodePopup")) {
        auto addNode = [&](const char* label, MeshType mt, NodeType nt = NodeType::Mesh) {
            if (ImGui::MenuItem(label)) {
                if (m_saveState) m_saveState();
                auto* n = scene.createNode(label);
                n->meshType = mt;
                n->nodeType = nt;
                if (nt == NodeType::DirectionalLight) {
                    n->meshType = MeshType::None;
                    n->transform.rotation = glm::vec3(-50.0f, -30.0f, 0.0f);
                }
            }
        };
        addNode("Empty", MeshType::None);
        addNode("Cube", MeshType::Cube);
        addNode("Plane", MeshType::Plane);
        addNode("Sphere", MeshType::Sphere);
        addNode("Quad", MeshType::Quad);
        ImGui::Separator();
        addNode("Directional Light", MeshType::None, NodeType::DirectionalLight);
        ImGui::EndPopup();
    }

    m_nodesToDelete.clear();
    m_reparentNode = nullptr;
    m_reparentTarget = nullptr;
    m_reparentIndex = -1;

    // Draw insert target before first child
    auto& rootChildren = scene.getRoot()->getChildren();
    drawInsertTarget(scene.getRoot(), 0, "##insert_root_0");

    for (int i = 0; i < static_cast<int>(rootChildren.size()); i++) {
        drawNodeTree(rootChildren[i].get(), scene, i);
        // Draw insert target after each child
        char id[32];
        snprintf(id, sizeof(id), "##insert_root_%d", i + 1);
        drawInsertTarget(scene.getRoot(), i + 1, id);
    }

    // Deferred reparent
    if (m_reparentNode && m_reparentTarget) {
        if (m_saveState) m_saveState();
        std::string serialized = scene.serializeNodeToString(m_reparentNode);
        // Adjust index if removing from same parent before the insert position
        int adjustedIndex = m_reparentIndex;
        if (m_reparentIndex >= 0 && m_reparentNode->getParent() == m_reparentTarget) {
            int oldIndex = m_reparentTarget->getChildIndex(m_reparentNode);
            if (oldIndex >= 0 && oldIndex < m_reparentIndex)
                adjustedIndex--;
        }
        scene.removeNode(m_reparentNode);
        Node* newNode = scene.deserializeNodeFromString(serialized, m_reparentTarget, adjustedIndex);
        // Select the reparented node and expand parents to reveal it
        if (newNode) {
            scene.selectNode(newNode, false);
            // Open all ancestor tree nodes so it's visible
            Node* ancestor = newNode->getParent();
            while (ancestor && ancestor != scene.getRoot()) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                // Force open by storing in ImGui state using the node pointer as ID
                ImGuiID id = ImGui::GetID((void*)(intptr_t)ancestor);
                ImGui::GetStateStorage()->SetInt(id, 1);
                ancestor = ancestor->getParent();
            }
        }
        m_reparentNode = nullptr;
        m_reparentTarget = nullptr;
    }

    // Deferred delete
    if (!m_nodesToDelete.empty()) {
        if (m_saveState) m_saveState();
        for (auto* node : m_nodesToDelete)
            scene.removeNode(node);
        m_nodesToDelete.clear();
    }

    // Delete key
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        auto selected = scene.getSelectedNodes();
        if (!selected.empty()) {
            if (m_saveState) m_saveState();
            for (auto* node : selected)
                scene.removeNode(node);
        }
    }

    // Click empty space to deselect
    if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
        scene.clearSelection();

    ImGui::End();
}

void HierarchyPanel::drawInsertTarget(Node* parent, int insertIndex, const char* id) {
    // Thin invisible rect as drop target between nodes
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 3.0f));
    if (ImGui::BeginDragDropTarget()) {
        // Visual feedback: draw a blue line
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(min.x, (min.y + max.y) * 0.5f),
            ImVec2(max.x, (min.y + max.y) * 0.5f),
            IM_COL32(80, 140, 255, 255), 2.0f);

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
            Node* draggedNode = *(Node**)payload->Data;
            if (draggedNode && draggedNode->getParent() != nullptr) {
                // Prevent dropping onto itself
                bool isAncestor = false;
                Node* check = parent;
                while (check) {
                    if (check == draggedNode) { isAncestor = true; break; }
                    check = check->getParent();
                }
                if (!isAncestor) {
                    m_reparentNode = draggedNode;
                    m_reparentTarget = parent;
                    m_reparentIndex = insertIndex;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
}

void HierarchyPanel::drawNodeTree(Node* node, Scene& scene, int childIndex) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (scene.isNodeSelected(node))
        flags |= ImGuiTreeNodeFlags_Selected;
    if (node->getChildren().empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    const char* icon = (node->nodeType == NodeType::DirectionalLight) ? "[L] " : "";
    bool opened = ImGui::TreeNodeEx((void*)(intptr_t)node, flags, "%s%s", icon, node->name.c_str());

    // Selection: click, ctrl+click, shift+click
    if (ImGui::IsItemClicked()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            if (scene.isNodeSelected(node))
                scene.deselectNode(node);
            else
                scene.selectNode(node, true);
        } else if (io.KeyShift && m_lastClickedNode) {
            scene.selectRange(m_lastClickedNode, node);
        } else {
            scene.selectNode(node, false);
        }
        m_lastClickedNode = node;
    }

    // Drag source
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("HIERARCHY_NODE", &node, sizeof(Node*));
        ImGui::Text("%s", node->name.c_str());
        ImGui::EndDragDropSource();
    }

    // Drop target on node: become child of this node
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
            Node* draggedNode = *(Node**)payload->Data;
            if (draggedNode && draggedNode != node && draggedNode->getParent() != nullptr) {
                bool isAncestor = false;
                Node* check = node;
                while (check) {
                    if (check == draggedNode) { isAncestor = true; break; }
                    check = check->getParent();
                }
                if (!isAncestor) {
                    m_reparentNode = draggedNode;
                    m_reparentTarget = node;
                    m_reparentIndex = -1; // append as last child
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Right-click context menu
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Create Empty Child")) {
            if (m_saveState) m_saveState();
            node->addChild("New Node");
        }
        if (ImGui::MenuItem("Duplicate")) {
            if (m_saveState) m_saveState();
            std::string serialized = scene.serializeNodeToString(node);
            scene.deserializeNodeFromString(serialized, node->getParent());
        }
        if (ImGui::MenuItem("Delete")) {
            m_nodesToDelete.push_back(node);
        }
        ImGui::EndPopup();
    }

    if (opened && !(flags & ImGuiTreeNodeFlags_Leaf)) {
        auto& children = node->getChildren();
        // Insert targets between children
        drawInsertTarget(node, 0, "##insert_first");
        for (int i = 0; i < static_cast<int>(children.size()); i++) {
            drawNodeTree(children[i].get(), scene, i);
            char id[32];
            snprintf(id, sizeof(id), "##insert_%d", i + 1);
            drawInsertTarget(node, i + 1, id);
        }
        ImGui::TreePop();
    }
}

} // namespace QymEngine
