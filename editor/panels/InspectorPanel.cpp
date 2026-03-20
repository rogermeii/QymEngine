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
