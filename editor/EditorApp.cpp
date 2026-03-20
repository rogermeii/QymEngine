#include "EditorApp.h"
#include "core/Log.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <renderdoc_app.h>
#include <GLFW/glfw3.h>

namespace QymEngine {

EditorApp::EditorApp()
    : Application({"QymEngine Editor", 1280, 720, true})
{
    // Must load RenderDoc BEFORE Vulkan initialization
    initRenderDoc();
}

void EditorApp::onInit()
{
    m_renderer.init(getWindow());
    m_renderer.setCamera(&m_camera);
    m_imguiLayer.init(m_renderer);

    m_renderer.setSwapChainRecreatedCallback([this]() {
        m_imguiLayer.onSwapChainRecreated(m_renderer);
    });

    std::string scenePath = std::string(ASSETS_DIR) + "/scenes/default.json";
    std::ifstream check(scenePath);
    if (check.good()) {
        check.close();
        m_scene.deserialize(scenePath);
        Log::info("Loaded scene: " + scenePath);
    } else {
        auto* node = m_scene.createNode("Cube");
        node->meshType = MeshType::Cube;
        Log::info("Created default scene");
    }

    m_consolePanel.init();
    m_modelPreview.init(m_renderer);

    if (m_rdocApi)
        Log::info("RenderDoc: ready (press F12 to capture)");

    Log::info("QymEngine Editor initialized");
}

void EditorApp::onUpdate()
{
    if (!m_renderer.beginFrame())
        return;

    // Apply any pending offscreen resize before rendering the scene
    m_sceneViewPanel.applyPendingResize(m_renderer);

    m_renderer.drawScene(m_scene);

    // Render model preview (after main scene, before ImGui)
    {
        Node* selected = m_scene.getSelectedNode();
        if (selected) {
            if (!selected->meshPath.empty()) {
                auto* mesh = m_renderer.getAssetManager().loadMesh(selected->meshPath);
                if (mesh)
                    m_modelPreview.renderMesh(m_renderer, mesh);
            } else if (selected->meshType != MeshType::None) {
                m_modelPreview.renderBuiltIn(m_renderer, selected->meshType);
            }
        }
    }

    m_imguiLayer.beginFrame();

    // Enable docking and set up layout
    setupDockingLayout();

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Scene"))
                m_scene.serialize(std::string(ASSETS_DIR) + "/scenes/default.json");
            if (ImGui::MenuItem("Load Scene"))
                m_scene.deserialize(std::string(ASSETS_DIR) + "/scenes/default.json");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            if (ImGui::MenuItem("Capture Frame (F12)", "F12", false, m_rdocApi != nullptr))
                m_captureRequested = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // F12 shortcut for RenderDoc capture
    if (ImGui::IsKeyPressed(ImGuiKey_F12) && m_rdocApi)
        m_captureRequested = true;

    // File-based external trigger: captures/trigger
    checkExternalCaptureTrigger();

    // Auto-capture for --capture-and-exit mode
    m_frameCount++;
    if (m_captureAndExit && !m_autoCaptureDone && m_rdocApi && m_frameCount == 5) {
        m_captureRequested = true;
        m_autoCaptureDone = true;
    }

    // Render all panels
    m_sceneViewPanel.onImGuiRender(m_renderer, m_camera, m_scene);
    m_hierarchyPanel.onImGuiRender(m_scene);
    m_inspectorPanel.onImGuiRender(m_scene, m_renderer.getAssetManager(), m_modelPreview);
    m_projectPanel.onImGuiRender();
    m_consolePanel.onImGuiRender();

    m_imguiLayer.endFrame(m_renderer.getCurrentCommandBuffer(),
                          m_renderer.getImageIndex());

    // RenderDoc frame capture (wrap the submit+present)
    if (m_captureRequested && m_rdocApi) {
        m_rdocApi->StartFrameCapture(nullptr, nullptr);
    }

    m_renderer.endFrame();

    if (m_captureRequested && m_rdocApi) {
        m_rdocApi->EndFrameCapture(nullptr, nullptr);
        m_captureRequested = false;

        uint32_t numCaptures = m_rdocApi->GetNumCaptures();
        if (numCaptures > 0) {
            char path[512];
            uint32_t pathLen = sizeof(path);
            m_rdocApi->GetCapture(numCaptures - 1, path, &pathLen, nullptr);
            Log::info(std::string("RenderDoc: captured to ") + path);

            // Write capture result for external tools
            {
                std::string resultPath = std::string(ASSETS_DIR) + "/../captures/result.txt";
                std::ofstream resultFile(resultPath);
                resultFile << path;
            }

            if (m_captureAndExit) {
                // Write capture path to output file for analysis script
                std::string outPath = m_captureOutputPath.empty()
                    ? std::string(ASSETS_DIR) + "/../capture_path.txt"
                    : m_captureOutputPath;
                std::ofstream out(outPath);
                out << path;
                out.close();
                Log::info("RenderDoc: capture-and-exit mode, shutting down");
                glfwSetWindowShouldClose(m_window->getNativeWindow(), GLFW_TRUE);
            } else {
                // Auto-open in RenderDoc UI
                if (!m_rdocApi->IsTargetControlConnected()) {
                    m_rdocApi->LaunchReplayUI(1, nullptr);
                }
            }
        }
    }
}

void EditorApp::onShutdown()
{
    vkDeviceWaitIdle(m_renderer.getContext().getDevice());
    m_modelPreview.shutdown(m_renderer.getContext().getDevice());
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

void EditorApp::setCaptureAndExit(bool enabled, const std::string& outputPath)
{
    m_captureAndExit = enabled;
    m_captureOutputPath = outputPath;
}

void EditorApp::initRenderDoc()
{
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod) {
        // Try to load from default install path
        mod = LoadLibraryA("C:/Program Files/RenderDoc/renderdoc.dll");
    }
    if (!mod) {
        Log::warn("RenderDoc: not found (install RenderDoc or launch from RenderDoc to enable capture)");
        return;
    }

    pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
    if (!RENDERDOC_GetAPI) {
        Log::warn("RenderDoc: failed to get API entry point");
        return;
    }

    int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void**)&m_rdocApi);
    if (ret != 1 || !m_rdocApi) {
        Log::warn("RenderDoc: failed to initialize API");
        m_rdocApi = nullptr;
        return;
    }

    // Disable RenderDoc's own capture key (we use our own F12)
    m_rdocApi->SetCaptureKeys(nullptr, 0);
    m_rdocApi->MaskOverlayBits(~RENDERDOC_OverlayBits::eRENDERDOC_Overlay_None,
                                RENDERDOC_OverlayBits::eRENDERDOC_Overlay_None);

    // Set capture file path prefix
    std::string capturePath = std::string(ASSETS_DIR) + "/../captures/qymengine";
    m_rdocApi->SetCaptureFilePathTemplate(capturePath.c_str());

    Log::info("RenderDoc: initialized (press F12 to capture frame)");
}

void EditorApp::captureFrame()
{
    if (!m_rdocApi) return;
    m_captureRequested = true;
}

void EditorApp::checkExternalCaptureTrigger()
{
    if (!m_rdocApi) return;

    std::string triggerPath = std::string(ASSETS_DIR) + "/../captures/trigger";
    std::ifstream check(triggerPath);
    if (check.good()) {
        check.close();
        // Delete trigger file immediately
        std::remove(triggerPath.c_str());
        m_captureRequested = true;
        Log::info("RenderDoc: external capture triggered");
    }
}

} // namespace QymEngine
