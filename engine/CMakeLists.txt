# engine/CMakeLists.txt

# 引擎静态库
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scene/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/asset/*.cpp"
)
add_library(QymEngineLib STATIC ${ENGINE_SOURCES})
target_include_directories(QymEngineLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(QymEngineLib PUBLIC Vulkan::Vulkan SDL2::SDL2-static glm stb nlohmann_json tinyobjloader)
target_compile_definitions(QymEngineLib PUBLIC
    $<$<BOOL:${ANDROID}>:VK_USE_PLATFORM_ANDROID_KHR>
    $<$<NOT:$<BOOL:${ANDROID}>>:VK_USE_PLATFORM_WIN32_KHR>
    SDL_MAIN_HANDLED
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

# 临时：现有 main.cpp 直接编译为可执行文件
add_executable(QymEngine "${CMAKE_SOURCE_DIR}/QymEngine/main.cpp")
target_link_libraries(QymEngine PRIVATE QymEngineLib)
