#include "Application.h"

namespace QymEngine {

Application::Application(const WindowProps& props)
    : m_window(std::make_unique<Window>(props))
{
}

void Application::run()
{
    onInit();

    while (!m_window->shouldClose())
    {
        m_window->pollEvents();
        onUpdate();
    }

    onShutdown();
}

} // namespace QymEngine
