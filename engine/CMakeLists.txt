add_executable(QymEngine "${CMAKE_SOURCE_DIR}/QymEngine/main.cpp")

target_link_libraries(QymEngine PRIVATE
    Vulkan::Vulkan
    glfw
    glm
    stb
)

target_compile_definitions(QymEngine PRIVATE
    VK_USE_PLATFORM_WIN32_KHR
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)
