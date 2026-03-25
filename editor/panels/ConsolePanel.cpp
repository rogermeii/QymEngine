#include "ConsolePanel.h"
#include "UIAutomation.h"
#include <imgui.h>

namespace QymEngine {

void ConsolePanel::init()
{
    Log::addCallback([this](const LogEntry& entry) {
        m_logs.push_back(entry);
    });
}

void ConsolePanel::onImGuiRender()
{
    ImGui::Begin("Console");
#ifndef __ANDROID__
    UIAutomation::recordPanel("Console");
#endif
    if (ImGui::Button("Clear"))
        m_logs.clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::Separator();

    ImGui::BeginChild("LogRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto& entry : m_logs) {
        ImVec4 color;
        switch (entry.level) {
            case LogLevel::Info:  color = ImVec4(1, 1, 1, 1); break;
            case LogLevel::Warn:  color = ImVec4(1, 1, 0, 1); break;
            case LogLevel::Error: color = ImVec4(1, 0.4f, 0.4f, 1); break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(entry.message.c_str());
        ImGui::PopStyleColor();
    }
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace QymEngine
