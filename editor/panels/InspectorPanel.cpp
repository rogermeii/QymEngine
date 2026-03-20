#include "InspectorPanel.h"
#include <imgui.h>

namespace QymEngine {

void InspectorPanel::onImGuiRender()
{
    ImGui::Begin("Inspector");
    ImGui::Text("Triangle");
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Position: 0.0, 0.0, 0.0");
        ImGui::Text("Rotation: 0.0, 0.0, 0.0");
        ImGui::Text("Scale:    1.0, 1.0, 1.0");
    }
    ImGui::End();
}

} // namespace QymEngine
