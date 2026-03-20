#include "HierarchyPanel.h"
#include <imgui.h>

namespace QymEngine {

void HierarchyPanel::onImGuiRender()
{
    ImGui::Begin("Hierarchy");
    ImGui::Text("Scene");
    ImGui::Indent();
    ImGui::Selectable("Main Camera");
    ImGui::Selectable("Directional Light");
    ImGui::Selectable("Triangle");
    ImGui::Unindent();
    ImGui::End();
}

} // namespace QymEngine
