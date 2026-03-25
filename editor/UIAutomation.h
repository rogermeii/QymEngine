#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>

struct SDL_Window;

namespace QymEngine {

class Renderer;
class Scene;
class Camera;

struct WidgetRect {
    std::string id;     // e.g. "hierarchy/Ground"
    std::string label;  // display text
    float x, y, w, h;  // screen coordinates
};

class UIAutomation {
public:
    // Editor operation callbacks (set by EditorApp)
    using VoidFn = std::function<void()>;
    using BoolFn = std::function<bool()>;
    using StringFn = std::function<std::string()>;
    using SaveFn = std::function<void(const std::string&)>;

    void setUndoFn(VoidFn fn) { m_undoFn = std::move(fn); }
    void setRedoFn(VoidFn fn) { m_redoFn = std::move(fn); }
    void setSaveSceneFn(VoidFn fn) { m_saveSceneFn = std::move(fn); }
    void setSaveSceneAsFn(SaveFn fn) { m_saveSceneAsFn = std::move(fn); }
    void setNewSceneFn(VoidFn fn) { m_newSceneFn = std::move(fn); }
    void setCanUndoFn(BoolFn fn) { m_canUndoFn = std::move(fn); }
    void setCanRedoFn(BoolFn fn) { m_canRedoFn = std::move(fn); }
    void setIsDirtyFn(BoolFn fn) { m_isDirtyFn = std::move(fn); }
    void setScenePathFn(StringFn fn) { m_scenePathFn = std::move(fn); }
    void setCaptureFrameFn(VoidFn fn) { m_captureFrameFn = std::move(fn); }
    void setGetGizmoModeFn(std::function<std::string()> fn) { m_getGizmoModeFn = std::move(fn); }
    void setSetGizmoModeFn(std::function<void(const std::string&)> fn) { m_setGizmoModeFn = std::move(fn); }

    // Call each frame to check for and execute commands
    void pollAndExecute(Renderer& renderer, Scene& scene, Camera& camera, SDL_Window* window);

    // Widget recording -- called by panels during ImGui rendering
    static void recordWidget(const std::string& id, const std::string& label);
    static void recordPanel(const std::string& name);
    // Called at frame start to clear previous frame's data
    static void beginFrame();

private:
    void executeCommand(const std::string& json, Renderer& renderer, Scene& scene, Camera& camera, SDL_Window* window);
    void writeResult(const std::string& json);

    // Input injection helpers
    void injectMouseClick(int x, int y, int button, SDL_Window* window);
    void injectMouseDoubleClick(int x, int y, SDL_Window* window);
    void injectMouseDrag(int fromX, int fromY, int toX, int toY, int button, SDL_Window* window);
    void injectMouseScroll(int x, int y, int delta, SDL_Window* window);
    void injectKeyPress(const std::string& key, bool ctrl, bool shift, bool alt, SDL_Window* window);
    void injectTextInput(const std::string& text, SDL_Window* window);

    // Screenshot implementation
    bool saveScreenshot(Renderer& renderer, const std::string& path);

    // Process deferred events (key up, mouse up from previous frame)
    void processDeferredEvents(SDL_Window* window);

    bool m_initialized = false;
    std::string m_commandPath;
    std::string m_resultPath;

    // Deferred SDL events to push next frame (for cross-frame input)
    struct DeferredEvent {
        uint32_t type;
        int x, y;
        uint32_t button;
        int32_t keycode;    // SDL_Keycode
        uint32_t scancode;  // SDL_Scancode
        uint16_t mod;
    };
    std::vector<DeferredEvent> m_deferredEvents;

    // Editor operation callbacks
    VoidFn m_undoFn, m_redoFn, m_saveSceneFn, m_newSceneFn;
    SaveFn m_saveSceneAsFn;
    VoidFn m_captureFrameFn;
    std::function<std::string()> m_getGizmoModeFn;
    std::function<void(const std::string&)> m_setGizmoModeFn;
    BoolFn m_canUndoFn, m_canRedoFn, m_isDirtyFn;
    StringFn m_scenePathFn;

    // Static widget registry (written by panels, read by query command)
    static std::vector<WidgetRect> s_widgets;
    struct PanelRect { float x, y, w, h; };
    static std::map<std::string, PanelRect> s_panels;
};

} // namespace QymEngine
