cmake_minimum_required(VERSION 3.22)
project(QymEngineAndroid LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Empty ASSETS_DIR on Android (SDL_RWFromFile reads from APK assets)
add_compile_definitions(ASSETS_DIR="")

# Vulkan (from NDK)
find_package(Vulkan REQUIRED)

# GLM (header-only)
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/glm-1.0.1-light")

# STB (header-only)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/stb-master")

# nlohmann/json (header-only)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/nlohmann")

# tinyobjloader (header-only)
add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/tinyobjloader")

# SDL2 (build from source for Android)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../build3/_deps/sdl2-src" "${CMAKE_BINARY_DIR}/sdl2-build")

# ImGui (with SDL2 + Vulkan backends)
file(GLOB IMGUI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/imgui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/imgui/backends/imgui_impl_sdl2.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/imgui/backends/imgui_impl_vulkan.cpp"
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/imgui/backends"
)
target_sources(imgui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/imguizmo/ImGuizmo.cpp")
target_include_directories(imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/imguizmo")
target_link_libraries(imgui PUBLIC Vulkan::Vulkan SDL2::SDL2)

# Engine library
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine/renderer/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine/scene/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine/asset/*.cpp"
)

add_library(QymEngineLib STATIC ${ENGINE_SOURCES})
target_include_directories(QymEngineLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../engine")
target_link_libraries(QymEngineLib PUBLIC
    Vulkan::Vulkan
    SDL2::SDL2
    glm stb nlohmann_json tinyobjloader
)
target_compile_definitions(QymEngineLib PUBLIC
    VK_USE_PLATFORM_ANDROID_KHR
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

# Editor library (panels, ImGuiLayer, EditorApp)
file(GLOB_RECURSE EDITOR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../editor/*.cpp"
)
# Exclude editor's own main.cpp (we have our own)
list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/editor/main\\.cpp$")

add_library(QymEditorLib STATIC ${EDITOR_SOURCES})
target_include_directories(QymEditorLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../editor")
# RenderDoc header not needed on Android (excluded by #ifndef __ANDROID__)
target_link_libraries(QymEditorLib PUBLIC QymEngineLib imgui)

# Android app shared library
add_library(qymengine_android SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
)
target_include_directories(qymengine_android PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(qymengine_android PRIVATE
    QymEditorLib
    android
    log
)
