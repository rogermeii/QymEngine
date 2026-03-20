#include "ProjectPanel.h"
#include <imgui.h>

namespace QymEngine {

void ProjectPanel::onImGuiRender()
{
    ImGui::Begin("Project");
    ImGui::Text("assets/");
    ImGui::Indent();
    ImGui::Selectable("shaders/");
    ImGui::Selectable("textures/");
    ImGui::Unindent();
    ImGui::End();
}

} // namespace QymEngine
