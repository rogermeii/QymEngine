#pragma once
#include <SDL.h>
#include <SDL_vulkan.h>
#include <string>
#include <functional>

namespace QymEngine {

struct WindowProps {
    std::string title = "QymEngine";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool maximized = false;
};

class Window {
public:
    using FramebufferResizeCallback = std::function<void(int, int)>;
    using SDLEventCallback = std::function<void(const SDL_Event&)>;

    Window(const WindowProps& props = WindowProps{});
    ~Window();

    void pollEvents();
    bool shouldClose() const;
    void requestClose();

    SDL_Window* getNativeWindow() const { return m_window; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

    void setFramebufferResizeCallback(FramebufferResizeCallback cb) { m_resizeCallback = cb; }
    void setEventCallback(SDLEventCallback cb) { m_eventCallback = cb; }

private:
    SDL_Window* m_window = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_shouldClose = false;
    FramebufferResizeCallback m_resizeCallback;
    SDLEventCallback m_eventCallback;
};

} // namespace QymEngine
