// ============================================================================
// QymEngine iOS 入口
//
// 使用 SDL2 提供的 SDL_main 替换机制，在 iOS 上启动编辑器应用。
// Metal 后端 (dispatch type 5)。
// ============================================================================

#include "EditorApp.h"
#include "renderer/VkDispatch.h"
#include <SDL.h>
#include <iostream>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

extern "C" int SDL_main(int argc, char* argv[]) {
    // iOS 横屏模式
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    // 初始化 SDL 视频子系统
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("[QymEngine iOS] SDL_Init 失败: %s", SDL_GetError());
        return 1;
    }

    // 初始化 Metal 后端 (dispatch type 5)
    QymEngine::vkInitDispatch(5);
    SDL_Log("[QymEngine iOS] 使用 Metal 后端");

    try {
        QymEngine::EditorApp app(QymEngine::RenderBackend::Metal);
        app.run();
    } catch (const std::exception& e) {
        SDL_Log("[QymEngine iOS] 崩溃: %s", e.what());
        return 1;
    } catch (...) {
        SDL_Log("[QymEngine iOS] 崩溃: 未知异常");
        return 1;
    }
    return 0;
}
