#include "EditorApp.h"
#include "core/Log.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace QymEngine {

EditorApp::EditorApp()
    : Application({"QymEngine Editor", 1280, 720})
{
}

void EditorApp::onInit()
{
    m_renderer.init(getWindow());
    m_imguiLayer.init(m_renderer);

    m_renderer.setSwapChainRecreatedCallback([this]() {
        m_imguiLayer.onSwapChainRecreated(m_renderer);
    });

    m_consolePanel.init();

    Log::info("QymEngine Editor initialized");
}

void EditorApp::onUpdate()
{
    if (!m_renderer.beginFrame())
        return;

    // Apply any pending offscreen resize before rendering the scene
    m_sceneViewPanel.applyPendingResize(m_renderer);

    m_renderer.drawScene();

    m_imguiLayer.beginFrame();

    // Enable docking and set up layout
    setupDockingLayout();

    // Render all panels
    m_sceneViewPanel.onImGuiRender(m_renderer);
    m_hierarchyPanel.onImGuiRender();
    m_inspectorPanel.onImGuiRender();
    m_projectPanel.onImGuiRender();
    m_consolePanel.onImGuiRender();

    m_imguiLayer.endFrame(m_renderer.getCurrentCommandBuffer(),
                          m_renderer.getImageIndex());

    m_renderer.endFrame();
}

void EditorApp::onShutdown()
{
    vkDeviceWaitIdle(m_renderer.getContext().getDevice());
    m_sceneViewPanel.cleanup();   // free ImGui descriptor set before ImGui shutdown
    m_imguiLayer.shutdown();
    m_renderer.shutdown();
}

void EditorApp::setupDockingLayout()
{
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    // Only set up layout once on first frame
    if (m_firstFrame) {
        m_firstFrame = false;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        ImGuiID dock_main   = dockspace_id;
        ImGuiID dock_left   = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.2f, nullptr, &dock_main);
        ImGuiID dock_right  = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.25f, nullptr, &dock_main);
        ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.3f, nullptr, &dock_main);

        ImGui::DockBuilderDockWindow("Hierarchy", dock_left);
        ImGui::DockBuilderDockWindow("Scene View", dock_main);
        ImGui::DockBuilderDockWindow("Inspector", dock_right);
        ImGui::DockBuilderDockWindow("Project", dock_bottom);
        ImGui::DockBuilderDockWindow("Console", dock_bottom);
        ImGui::DockBuilderFinish(dockspace_id);
    }
}

} // namespace QymEngine
