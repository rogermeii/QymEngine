#include "EditorApp.h"
#include <iostream>
#include <cstring>

int main(int argc, char* argv[]) {
    QymEngine::EditorApp app;

    // Parse command line args
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--capture-and-exit") == 0) {
            std::string outputPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outputPath = argv[++i];
            }
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
