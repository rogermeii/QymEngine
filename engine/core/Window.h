#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <string>
#include <functional>

namespace QymEngine {

struct WindowProps {
    std::string title = "QymEngine";
    uint32_t width = 1280;
    uint32_t height = 720;
};

class Window {
public:
    using FramebufferResizeCallback = std::function<void(int, int)>;

    Window(const WindowProps& props = WindowProps{});
    ~Window();

    void pollEvents();
    bool shouldClose() const;

    GLFWwindow* getNativeWindow() const { return m_window; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

    void setFramebufferResizeCallback(FramebufferResizeCallback cb) { m_resizeCallback = cb; }

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* m_window = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    FramebufferResizeCallback m_resizeCallback;
};

} // namespace QymEngine
