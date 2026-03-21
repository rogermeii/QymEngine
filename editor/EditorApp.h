#pragma once

#include "core/Application.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "ImGuiLayer.h"
#include "UndoManager.h"
#include "Clipboard.h"

#include "panels/SceneViewPanel.h"
#include "panels/HierarchyPanel.h"
#include "panels/InspectorPanel.h"
#include "panels/ProjectPanel.h"
#include "panels/ConsolePanel.h"
#include "panels/ModelPreview.h"

#ifndef __ANDROID__
#include <renderdoc_app.h>
#include "UIAutomation.h"
#endif

namespace QymEngine {

class EditorApp : public Application {
public:
    EditorApp();
#ifndef __ANDROID__
    void setCaptureAndExit(bool enabled, const std::string& outputPath = "");
#endif

protected:
    void onInit() override;
    void onUpdate() override;
    void onShutdown() override;

private:
    void setupDockingLayout();
    void handleShortcuts();
#ifndef __ANDROID__
    void initRenderDoc();
    void captureFrame();
    void checkExternalCaptureTrigger();
#endif

    Renderer       m_renderer;
    Scene          m_scene;
    Camera         m_camera;
    ImGuiLayer     m_imguiLayer;
    UndoManager    m_undoManager;
    Clipboard      m_clipboard;

    SceneViewPanel m_sceneViewPanel;
    HierarchyPanel m_hierarchyPanel;
    InspectorPanel m_inspectorPanel;
    ProjectPanel   m_projectPanel;
    ConsolePanel   m_consolePanel;
    ModelPreview   m_modelPreview;

    bool m_firstFrame = true;
    std::string m_currentScenePath;
    bool m_showSaveAsPopup = false;
    char m_saveAsBuffer[256] = {};

#ifndef __ANDROID__
    RENDERDOC_API_1_6_0* m_rdocApi = nullptr;
    bool m_captureRequested = false;
    bool m_capturingThisFrame = false;
    int m_frameCount = 0;
    bool m_autoCaptureDone = false;
    bool m_captureAndExit = false;
    std::string m_captureOutputPath;

    UIAutomation m_uiAutomation;
#endif
};

} // namespace QymEngine
