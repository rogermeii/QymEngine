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
#include "panels/PostProcessPanel.h"

#if !defined(__ANDROID__) && !defined(__APPLE__)
#include <renderdoc_app.h>
#endif
#ifndef __ANDROID__
#include "UIAutomation.h"
#endif

#ifdef _WIN32
#include "renderer/d3d12/D3D12Context.h"
#endif

namespace QymEngine {

class EditorApp : public Application {
public:
    EditorApp(RenderBackend backend = RenderBackend::Vulkan);
    void setBindlessEnabled(bool enabled) { m_forceBindless = enabled; }
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

    SceneViewPanel    m_sceneViewPanel;
    HierarchyPanel    m_hierarchyPanel;
    InspectorPanel    m_inspectorPanel;
    ProjectPanel      m_projectPanel;
    ConsolePanel      m_consolePanel;
    ModelPreview      m_modelPreview;
    PostProcessPanel  m_postProcessPanel;

    bool m_firstFrame = true;
    std::string m_currentScenePath;
    bool m_showSaveAsPopup = false;
    bool m_showAbout = false;
    char m_saveAsBuffer[256] = {};
    bool m_sceneDirty = false;

#ifndef __ANDROID__
#if defined(_WIN32)
    RENDERDOC_API_1_6_0* m_rdocApi = nullptr;
#else
    void* m_rdocApi = nullptr;
#endif
    bool m_captureRequested = false;
    bool m_capturingThisFrame = false;
    int m_frameCount = 0;
    bool m_autoCaptureDone = false;
    bool m_captureAndExit = false;
    std::string m_captureOutputPath;

    UIAutomation m_uiAutomation;
#endif

    bool m_forceBindless = false;

#ifdef _WIN32
    D3D12Context m_d3d12Context;
#endif
};

} // namespace QymEngine
