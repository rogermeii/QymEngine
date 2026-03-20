#include "HierarchyPanel.h"
#include <imgui.h>

namespace QymEngine {

void HierarchyPanel::onImGuiRender(Scene& scene) {
    ImGui::Begin("Hierarchy");

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

    m_nodeToDelete = nullptr;

    for (auto& child : scene.getRoot()->getChildren())
        drawNodeTree(child.get(), scene);

    // Deferred delete after iteration
    if (m_nodeToDelete) {
        scene.removeNode(m_nodeToDelete);
        m_nodeToDelete = nullptr;
    }

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
            m_nodeToDelete = node;
        ImGui::EndPopup();
    }

    if (opened && !(flags & ImGuiTreeNodeFlags_Leaf)) {
        for (auto& child : node->getChildren())
            drawNodeTree(child.get(), scene);
        ImGui::TreePop();
    }
}

} // namespace QymEngine
