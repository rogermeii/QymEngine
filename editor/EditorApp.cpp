#include "EditorApp.h"
#include "core/Log.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl2.h>
#include <SDL.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <chrono>

#ifndef __ANDROID__
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <renderdoc_app.h>
#endif

namespace QymEngine {

EditorApp::EditorApp()
    : Application({"QymEngine Editor", 1280, 720, true})
{
#ifndef __ANDROID__
    // Must load RenderDoc BEFORE Vulkan initialization
    initRenderDoc();
#endif
}

void EditorApp::onInit()
{
    m_renderer.init(getWindow());
    m_renderer.setCamera(&m_camera);
    m_imguiLayer.init(m_renderer);

    m_renderer.setSwapChainRecreatedCallback([this]() {
        m_imguiLayer.onSwapChainRecreated(m_renderer);
    });

    // Override event callback to forward touch events to SceneViewPanel
    getWindow().setEventCallback([this](const SDL_Event& event) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        m_sceneViewPanel.processEvent(event);
    });

#ifdef __ANDROID__
    m_scene.deserialize("scenes/default.json");
    Log::info("Loaded scene from assets");
#else
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
#endif

    m_consolePanel.init();
    m_modelPreview.init(m_renderer);

    // Initialize undo manager (preserve selection across undo/redo)
    m_undoManager.init(
        [this]() {
            // Encode selected node name into the snapshot
            std::string selectedName;
            if (m_scene.getSelectedNode())
                selectedName = m_scene.getSelectedNode()->name;
            return selectedName + "\n" + m_scene.toJsonString();
        },
        [this](const std::string& snapshot) {
            // Extract selected node name and scene JSON
            size_t sep = snapshot.find('\n');
            std::string selectedName = snapshot.substr(0, sep);
            std::string json = snapshot.substr(sep + 1);
            m_scene.fromJsonString(json);
            // Re-select node by name
            if (!selectedName.empty()) {
                m_scene.traverseNodes([&](Node* n) {
                    if (n->name == selectedName)
                        m_scene.selectNode(n, false);
                });
            }
        }
    );

    // Connect panels to undo
    m_hierarchyPanel.setSaveStateFn([this]() { m_undoManager.saveState(); });
    m_inspectorPanel.setSaveStateFn([this]() { m_undoManager.saveState(); });
    m_sceneViewPanel.setSaveStateFn([this]() { m_undoManager.saveState(); });

#ifdef __ANDROID__
    m_currentScenePath = "scenes/default.json";
#else
    m_currentScenePath = std::string(ASSETS_DIR) + "/scenes/default.json";
#endif

#ifndef __ANDROID__
    if (m_rdocApi)
        Log::info("RenderDoc: ready (press F12 to capture)");
#endif

    Log::info("QymEngine Editor initialized");
}

void EditorApp::onUpdate()
{
#ifndef __ANDROID__
    // Check file-based capture trigger and F12 BEFORE starting capture
    checkExternalCaptureTrigger();

    // Start RenderDoc capture BEFORE beginFrame so entire frame is captured
    m_capturingThisFrame = m_captureRequested && m_rdocApi;
    if (m_capturingThisFrame) {
        m_captureRequested = false;
        // Clean old captures (keep last 5)
        {
            std::vector<std::string> rdcFiles;
            std::string captureDir = std::string(ASSETS_DIR) + "/../captures";
            for (auto& entry : std::filesystem::directory_iterator(captureDir)) {
                if (entry.path().extension() == ".rdc")
                    rdcFiles.push_back(entry.path().string());
            }
            std::sort(rdcFiles.begin(), rdcFiles.end());
            while (rdcFiles.size() > 5) {
                std::filesystem::remove(rdcFiles.front());
                rdcFiles.erase(rdcFiles.begin());
            }
        }
        // Set unique capture path with timestamp
        {
            auto now = std::chrono::system_clock::now();
            auto epoch = now.time_since_epoch();
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
            std::string capturePath = std::string(ASSETS_DIR) + "/../captures/qymengine_" + std::to_string(secs);
            m_rdocApi->SetCaptureFilePathTemplate(capturePath.c_str());
        }
        m_rdocApi->StartFrameCapture(nullptr, nullptr);
    }
#endif

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
        } else if (m_projectPanel.isSelectedModel()) {
            // Preview model selected in Project panel
            auto* mesh = m_renderer.getAssetManager().loadMesh(m_projectPanel.getSelectedFile());
            if (mesh)
                m_modelPreview.renderMesh(m_renderer, mesh);
        }
    }

    m_imguiLayer.beginFrame();

    // Enable docking and set up layout
    setupDockingLayout();

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
                m_undoManager.saveState();
                m_scene.clearSelection();
                m_scene.fromJsonString("{\"scene\":{\"name\":\"Untitled\",\"nodes\":[]}}");
                m_currentScenePath.clear();
            }
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                if (!m_currentScenePath.empty())
                    m_scene.serialize(m_currentScenePath);
                else
                    m_showSaveAsPopup = true;
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
                m_showSaveAsPopup = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load Scene")) {
                m_undoManager.saveState();
#ifdef __ANDROID__
                m_scene.deserialize("scenes/default.json");
#else
                m_scene.deserialize(std::string(ASSETS_DIR) + "/scenes/default.json");
#endif
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_undoManager.canUndo()))
                m_undoManager.undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_undoManager.canRedo()))
                m_undoManager.redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, m_scene.getSelectedNode() != nullptr)) {
                std::vector<std::string> jsons;
                for (auto* n : m_scene.getSelectedNodes())
                    jsons.push_back(m_scene.serializeNodeToString(n));
                m_clipboard.copyNodes(jsons);
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, m_clipboard.hasContent())) {
                m_undoManager.saveState();
                Node* parent = m_scene.getSelectedNode()
                    ? m_scene.getSelectedNode()->getParent()
                    : m_scene.getRoot();
                for (auto& json : m_clipboard.getNodes())
                    m_scene.deserializeNodeFromString(json, parent);
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_scene.getSelectedNode() != nullptr)) {
                m_undoManager.saveState();
                for (auto* n : m_scene.getSelectedNodes()) {
                    std::string json = m_scene.serializeNodeToString(n);
                    m_scene.deserializeNodeFromString(json, n->getParent());
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del", false, m_scene.getSelectedNode() != nullptr)) {
                m_undoManager.saveState();
                auto selected = m_scene.getSelectedNodes();
                for (auto* n : selected)
                    m_scene.removeNode(n);
            }
            ImGui::EndMenu();
        }
#ifndef __ANDROID__
        if (ImGui::BeginMenu("Debug")) {
            if (ImGui::MenuItem("Capture Frame (F12)", "F12", false, m_rdocApi != nullptr))
                m_captureRequested = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Reload Shaders", "Ctrl+R")) {
                m_renderer.reloadShaders();
                Log::info("Shaders reloaded");
            }
            ImGui::EndMenu();
        }
#endif
        ImGui::EndMainMenuBar();
    }

    // Save As popup
    if (m_showSaveAsPopup) {
        ImGui::OpenPopup("Save Scene As");
        m_showSaveAsPopup = false;
    }
    if (ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("File name:");
        ImGui::InputText("##saveas", m_saveAsBuffer, sizeof(m_saveAsBuffer));
        if (ImGui::Button("Save")) {
            std::string filename = m_saveAsBuffer;
            if (!filename.empty()) {
                if (filename.find(".json") == std::string::npos)
                    filename += ".json";
                m_currentScenePath = std::string(ASSETS_DIR) + "/scenes/" + filename;
                m_scene.serialize(m_currentScenePath);
                Log::info("Saved scene to: " + m_currentScenePath);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Keyboard shortcuts
    handleShortcuts();

#ifndef __ANDROID__
    // F12 shortcut for RenderDoc capture
    if (ImGui::IsKeyPressed(ImGuiKey_F12) && m_rdocApi)
        m_captureRequested = true;

    // Auto-capture for --capture-and-exit mode
    m_frameCount++;
    if (m_captureAndExit && !m_autoCaptureDone && m_rdocApi && m_frameCount == 5) {
        m_captureRequested = true;
        m_autoCaptureDone = true;
    }
#endif

    // Render all panels
    m_sceneViewPanel.onImGuiRender(m_renderer, m_camera, m_scene);
    m_hierarchyPanel.onImGuiRender(m_scene, &m_undoManager, &m_clipboard, &m_projectPanel);
    m_inspectorPanel.onImGuiRender(m_scene, m_renderer.getAssetManager(), m_modelPreview, m_projectPanel);
    m_projectPanel.onImGuiRender();
    m_consolePanel.onImGuiRender();

    m_imguiLayer.endFrame(m_renderer.getCurrentCommandBuffer(),
                          m_renderer.getImageIndex());

    m_renderer.endFrame();

#ifndef __ANDROID__
    if (m_capturingThisFrame) {
        m_rdocApi->EndFrameCapture(nullptr, nullptr);
        m_capturingThisFrame = false;

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
                std::string outPath = m_captureOutputPath.empty()
                    ? std::string(ASSETS_DIR) + "/../capture_path.txt"
                    : m_captureOutputPath;
                std::ofstream out(outPath);
                out << path;
                out.close();
                Log::info("RenderDoc: capture-and-exit mode, shutting down");
                m_window->requestClose();
            }
        }
    }
#endif
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

#ifndef __ANDROID__
void EditorApp::setCaptureAndExit(bool enabled, const std::string& outputPath)
{
    m_captureAndExit = enabled;
    m_captureOutputPath = outputPath;
}

void EditorApp::initRenderDoc()
{
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod) {
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

    m_rdocApi->SetCaptureKeys(nullptr, 0);
    m_rdocApi->MaskOverlayBits(~RENDERDOC_OverlayBits::eRENDERDOC_Overlay_None,
                                RENDERDOC_OverlayBits::eRENDERDOC_Overlay_None);

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
        std::remove(triggerPath.c_str());
        m_captureRequested = true;
        Log::info("RenderDoc: external capture triggered");
    }
}
#endif

void EditorApp::handleShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl;
    bool shift = io.KeyShift;

    // Ctrl+Z: Undo
    if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Z))
        m_undoManager.undo();

    // Ctrl+Y or Ctrl+Shift+Z: Redo
    if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
        (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_Z)))
        m_undoManager.redo();

    // Ctrl+C: Copy
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
        auto& selected = m_scene.getSelectedNodes();
        if (!selected.empty()) {
            std::vector<std::string> jsons;
            for (auto* n : selected)
                jsons.push_back(m_scene.serializeNodeToString(n));
            m_clipboard.copyNodes(jsons);
        }
    }

    // Ctrl+V: Paste
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V) && m_clipboard.hasContent()) {
        m_undoManager.saveState();
        Node* parent = m_scene.getSelectedNode()
            ? m_scene.getSelectedNode()->getParent()
            : m_scene.getRoot();
        for (auto& json : m_clipboard.getNodes())
            m_scene.deserializeNodeFromString(json, parent);
    }

    // Ctrl+D: Duplicate
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
        auto& selected = m_scene.getSelectedNodes();
        if (!selected.empty()) {
            m_undoManager.saveState();
            // Copy first to avoid modifying the set while iterating
            std::vector<Node*> nodes(selected.begin(), selected.end());
            for (auto* n : nodes) {
                std::string json = m_scene.serializeNodeToString(n);
                m_scene.deserializeNodeFromString(json, n->getParent());
            }
        }
    }

    // Ctrl+N: New Scene
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N)) {
        m_undoManager.saveState();
        m_scene.clearSelection();
        m_scene.fromJsonString("{\"scene\":{\"name\":\"Untitled\",\"nodes\":[]}}");
        m_currentScenePath.clear();
    }

    // Ctrl+S: Save
    if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (!m_currentScenePath.empty())
            m_scene.serialize(m_currentScenePath);
        else
            m_showSaveAsPopup = true;
    }

    // Ctrl+Shift+S: Save As
    if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_S))
        m_showSaveAsPopup = true;

    // Ctrl+R: Reload Shaders
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_R)) {
        m_renderer.reloadShaders();
        Log::info("Shaders reloaded");
    }
}

} // namespace QymEngine
