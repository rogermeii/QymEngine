#include "core/Application.h"
#include "renderer/Renderer.h"
#include "ImGuiLayer.h"

#include <imgui.h>
#include <iostream>

class EditorApp : public QymEngine::Application {
public:
    EditorApp() : Application({"QymEngine Editor", 1280, 720}) {}

protected:
    void onInit() override {
        m_renderer.init(getWindow());
        m_imguiLayer.init(m_renderer);

        m_renderer.setSwapChainRecreatedCallback([this]() {
            m_imguiLayer.onSwapChainRecreated(m_renderer);
        });
    }

    void onUpdate() override {
        if (!m_renderer.beginFrame())
            return;

        m_renderer.drawScene();

        m_imguiLayer.beginFrame();
        m_imguiLayer.enableDocking();
        ImGui::ShowDemoWindow();
        m_imguiLayer.endFrame(m_renderer.getCurrentCommandBuffer(),
                              m_renderer.getImageIndex());

        m_renderer.endFrame();
    }

    void onShutdown() override {
        vkDeviceWaitIdle(m_renderer.getContext().getDevice());
        m_imguiLayer.shutdown();
        m_renderer.shutdown();
    }

private:
    QymEngine::Renderer   m_renderer;
    QymEngine::ImGuiLayer m_imguiLayer;
};

int main() {
    EditorApp app;
    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
