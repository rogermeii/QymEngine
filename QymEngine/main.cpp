#include "core/Application.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include <iostream>
#include <stdexcept>

class TriangleApp : public QymEngine::Application {
public:
    TriangleApp() : Application({"QymEngine", 800, 600}) {}

protected:
    void onInit() override {
        m_renderer.init(getWindow());
        m_scene.createNode("Quad");
    }

    void onUpdate() override {
        if (m_renderer.beginFrame()) {
            m_renderer.drawScene(m_scene);
            m_renderer.endFrame();
        }
    }

    void onShutdown() override {
        m_renderer.shutdown();
    }

private:
    QymEngine::Renderer m_renderer;
    QymEngine::Scene    m_scene;
};

int main() {
    TriangleApp app;
    try { app.run(); }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
