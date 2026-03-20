cmake_minimum_required(VERSION 3.20)
project(QymEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Debug/Release 配置（MSVC 自动在 Release 下定义 NDEBUG，控制 validation layer 开关）
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

# 资源路径宏
add_compile_definitions(ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

# Vulkan
find_package(Vulkan REQUIRED)

# GLFW（预编译库）
add_library(glfw STATIC IMPORTED)
set_target_properties(glfw PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/3rd-party/glfw-3.4.bin.WIN64/lib-vc2022/glfw3.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/3rd-party/glfw-3.4.bin.WIN64/include"
)

# GLM（header-only）
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${CMAKE_SOURCE_DIR}/3rd-party/glm-1.0.1-light")

# STB（header-only）
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${CMAKE_SOURCE_DIR}/3rd-party/stb-master")

# ImGui（docking 分支）
file(GLOB IMGUI_SOURCES
    "3rd-party/imgui/*.cpp"
    "3rd-party/imgui/backends/imgui_impl_glfw.cpp"
    "3rd-party/imgui/backends/imgui_impl_vulkan.cpp"
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    "${CMAKE_SOURCE_DIR}/3rd-party/imgui"
    "${CMAKE_SOURCE_DIR}/3rd-party/imgui/backends"
)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan glfw)

# 引擎（暂时直接编译 main.cpp 为可执行文件）
add_subdirectory(engine)
