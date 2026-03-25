#include "EditorApp.h"
#include "renderer/VkDispatch.h"
#include <iostream>
#include <cstring>

int main(int argc, char* argv[]) {
    QymEngine::RenderBackend backend = QymEngine::RenderBackend::Vulkan;
    bool enableBindless = false;

    // Parse command line args
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--d3d12") == 0)
            backend = QymEngine::RenderBackend::D3D12;
        else if (std::strcmp(argv[i], "--d3d11") == 0)
            backend = QymEngine::RenderBackend::D3D11;
        else if (std::strcmp(argv[i], "--opengl") == 0)
            backend = QymEngine::RenderBackend::OpenGL;
        else if (std::strcmp(argv[i], "--gles") == 0)
            backend = QymEngine::RenderBackend::GLES;
        else if (std::strcmp(argv[i], "--metal") == 0)
            backend = QymEngine::RenderBackend::Metal;
        else if (std::strcmp(argv[i], "--bindless") == 0)
            enableBindless = true;
    }

    // 初始化 Vulkan 函数分发表（必须在任何 vk* 调用之前）
    // 0=Vulkan, 1=D3D12, 2=D3D11, 3=OpenGL, 4=GLES
    int backendType = (backend == QymEngine::RenderBackend::D3D12) ? 1
                    : (backend == QymEngine::RenderBackend::D3D11) ? 2
                    : (backend == QymEngine::RenderBackend::OpenGL) ? 3
                    : (backend == QymEngine::RenderBackend::GLES) ? 4
                    : (backend == QymEngine::RenderBackend::Metal) ? 5 : 0;
    QymEngine::vkInitDispatch(backendType);

    QymEngine::EditorApp app(backend);
    app.setBindlessEnabled(enableBindless);

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--capture-and-exit") == 0) {
            std::string outputPath;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                outputPath = argv[++i];
            app.setCaptureAndExit(true, outputPath);
        }
    }

    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
