#pragma once

#include "core/Application.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "ImGuiLayer.h"

#include "panels/SceneViewPanel.h"
#include "panels/HierarchyPanel.h"
#include "panels/InspectorPanel.h"
#include "panels/ProjectPanel.h"
#include "panels/ConsolePanel.h"

namespace QymEngine {

class EditorApp : public Application {
public:
    EditorApp();

protected:
    void onInit() override;
    void onUpdate() override;
    void onShutdown() override;

private:
    void setupDockingLayout();

    Renderer       m_renderer;
    Scene          m_scene;
    Camera         m_camera;
    ImGuiLayer     m_imguiLayer;

    SceneViewPanel m_sceneViewPanel;
    HierarchyPanel m_hierarchyPanel;
    InspectorPanel m_inspectorPanel;
    ProjectPanel   m_projectPanel;
    ConsolePanel   m_consolePanel;

    bool m_firstFrame = true;
};

} // namespace QymEngine
