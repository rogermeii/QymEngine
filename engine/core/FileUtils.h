#pragma once
#include <SDL.h>
#include <string>
#include <vector>
#include <stdexcept>

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

// Build asset path: on Android use relative path, on Windows use ASSETS_DIR prefix
inline std::string assetPath(const std::string& relativePath) {
#ifdef __ANDROID__
    return relativePath;
#else
    return std::string(ASSETS_DIR) + "/" + relativePath;
#endif
}

} // namespace QymEngine
