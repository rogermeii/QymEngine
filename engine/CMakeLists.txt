# engine/CMakeLists.txt

# 引擎静态库
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer/*.cpp"
)
add_library(QymEngineLib STATIC ${ENGINE_SOURCES})
target_include_directories(QymEngineLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(QymEngineLib PUBLIC Vulkan::Vulkan glfw glm stb)
target_compile_definitions(QymEngineLib PUBLIC
    VK_USE_PLATFORM_WIN32_KHR
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

# 临时：现有 main.cpp 直接编译为可执行文件
add_executable(QymEngine "${CMAKE_SOURCE_DIR}/QymEngine/main.cpp")
target_link_libraries(QymEngine PRIVATE QymEngineLib)
