#include "core/Application.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "scene/Camera.h"

#include <vulkan/vulkan.h>
#include <iostream>
#include <stdexcept>

// Standalone runtime: render scene to window without editor UI
class StandaloneApp : public QymEngine::Application {
public:
    StandaloneApp() : Application({"QymEngine", 1280, 720}) {}

protected:
    void onInit() override {
        m_renderer.init(getWindow());
        m_renderer.setCamera(&m_camera);

        m_scene.deserialize("scenes/default.json");

        m_camera.target = {0.0f, 0.0f, 0.0f};
        m_camera.distance = 8.0f;
        m_camera.yaw = -45.0f;
        m_camera.pitch = 30.0f;
    }

    void onUpdate() override {
        VkExtent2D ext = m_renderer.getSwapChain().getExtent();
        if (ext.width > 0 && ext.height > 0) {
            if (!m_renderer.isOffscreenReady() ||
                m_renderer.getOffscreenWidth() != ext.width ||
                m_renderer.getOffscreenHeight() != ext.height) {
                vkDeviceWaitIdle(m_renderer.getContext().getDevice());
                m_renderer.resizeOffscreen(ext.width, ext.height);
            }
        }

        if (!m_renderer.isOffscreenReady())
            return;

        if (m_renderer.beginFrame()) {
            m_renderer.drawScene(m_scene);
            m_renderer.blitToSwapchain();
            m_renderer.endFrame();
        }
    }

    void onShutdown() override {
        vkDeviceWaitIdle(m_renderer.getContext().getDevice());
        m_renderer.shutdown();
    }

private:
    QymEngine::Renderer m_renderer;
    QymEngine::Scene    m_scene;
    QymEngine::Camera   m_camera;
};

int main(int /*argc*/, char* /*argv*/[]) {
    StandaloneApp app;
    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "QymEngine crashed: " << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "QymEngine crashed: unknown exception" << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
