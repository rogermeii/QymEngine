#include "SceneViewPanel.h"
#include <imgui.h>

namespace QymEngine {

void SceneViewPanel::onImGuiRender()
{
    ImGui::Begin("Scene View");
    ImGui::Text("TODO: Offscreen render target");
    ImGui::End();
}

} // namespace QymEngine
