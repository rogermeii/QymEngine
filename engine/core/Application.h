#pragma once
#include "Window.h"
#include <memory>

namespace QymEngine {

class Application {
public:
    Application(const WindowProps& props = WindowProps{});
    virtual ~Application() = default;

    void run();

    Window& getWindow() { return *m_window; }

protected:
    virtual void onInit() {}
    virtual void onUpdate() {}
    virtual void onShutdown() {}

    std::unique_ptr<Window> m_window;
};

} // namespace QymEngine
