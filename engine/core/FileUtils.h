#pragma once
#include <SDL.h>
#include <string>
#include <vector>
#include <stdexcept>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace QymEngine {

// Read entire file as string (for JSON etc.)
inline std::string readFileAsString(const std::string& path) {
    SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
    if (!rw)
        throw std::runtime_error("Failed to open file: " + path + " (" + SDL_GetError() + ")");

    Sint64 size = SDL_RWsize(rw);
    if (size < 0) {
        SDL_RWclose(rw);
        throw std::runtime_error("Failed to get file size: " + path);
    }

    std::string content(static_cast<size_t>(size), '\0');
    SDL_RWread(rw, &content[0], 1, static_cast<size_t>(size));
    SDL_RWclose(rw);
    return content;
}

// Read entire file as binary bytes (for images, OBJ, etc.)
inline std::vector<unsigned char> readFileAsBytes(const std::string& path) {
    SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
    if (!rw)
        return {}; // Return empty on failure (caller checks)

    Sint64 size = SDL_RWsize(rw);
    if (size < 0) {
        SDL_RWclose(rw);
        return {};
    }

    std::vector<unsigned char> data(static_cast<size_t>(size));
    SDL_RWread(rw, data.data(), 1, static_cast<size_t>(size));
    SDL_RWclose(rw);
    return data;
}

// Check if file can be opened (replaces fs::exists for APK assets)
inline bool fileExists(const std::string& path) {
    SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
    if (rw) {
        SDL_RWclose(rw);
        return true;
    }
    return false;
}

// Build asset path:
// - Android: 使用相对路径（SDL_RWFromFile 直接访问 APK 内资源）
// - iOS: 使用 app bundle 内的 Resources/assets/ 路径
// - 其他平台: 使用 ASSETS_DIR 宏前缀
inline std::string assetPath(const std::string& relativePath) {
#ifdef __ANDROID__
    return relativePath;
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
    // iOS: 资源打包在 app bundle 的 Resources/assets/ 下
    static std::string basePath;
    if (basePath.empty()) {
        const char* base = SDL_GetBasePath();
        if (base) {
            basePath = std::string(base) + "assets";
            SDL_free((void*)base);
        } else {
            basePath = "assets";
        }
    }
    return basePath + "/" + relativePath;
#else
    return std::string(ASSETS_DIR) + "/" + relativePath;
#endif
}

} // namespace QymEngine
