#pragma once
#include <string>
#include <vector>
#include <map>

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

    bool m_initialized = false;
    std::string m_commandPath;
    std::string m_resultPath;

    // Static widget registry (written by panels, read by query command)
    static std::vector<WidgetRect> s_widgets;
    struct PanelRect { float x, y, w, h; };
    static std::map<std::string, PanelRect> s_panels;
};

} // namespace QymEngine
