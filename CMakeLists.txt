cmake_minimum_required(VERSION 3.20)
project(QymEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Debug/Release 配置（MSVC 自动在 Release 下定义 NDEBUG，控制 validation layer 开关）
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

# 资源路径宏
add_compile_definitions(ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

# Vulkan: 全局禁止函数原型声明，所有 vk* 调用通过 VkDispatch 函数指针
# ImGui 内建支持此模式（IMGUI_IMPL_VULKAN_USE_LOADER）
add_compile_definitions(VK_NO_PROTOTYPES)

# Vulkan
find_package(Vulkan REQUIRED)

# SDL2（通过 FetchContent 获取）
include(FetchContent)
FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.12
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# GLM（header-only）
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${CMAKE_SOURCE_DIR}/3rd-party/glm-1.0.1-light")

# STB（header-only）
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${CMAKE_SOURCE_DIR}/3rd-party/stb-master")

# nlohmann/json (header-only)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_SOURCE_DIR}/3rd-party/nlohmann")

# tinyobjloader (header-only)
add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE "${CMAKE_SOURCE_DIR}/3rd-party/tinyobjloader")

# ImGui（docking 分支）
file(GLOB IMGUI_SOURCES
    "3rd-party/imgui/*.cpp"
    "3rd-party/imgui/backends/imgui_impl_sdl2.cpp"
    "3rd-party/imgui/backends/imgui_impl_vulkan.cpp"
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    "${CMAKE_SOURCE_DIR}/3rd-party/imgui"
    "${CMAKE_SOURCE_DIR}/3rd-party/imgui/backends"
)
target_include_directories(imgui PUBLIC ${Vulkan_INCLUDE_DIRS})
target_link_libraries(imgui PUBLIC SDL2::SDL2-static)

# ImGuizmo (built as part of imgui since it depends on ImGui)
target_sources(imgui PRIVATE "3rd-party/imguizmo/ImGuizmo.cpp")
target_include_directories(imgui PUBLIC "${CMAKE_SOURCE_DIR}/3rd-party/imguizmo")

# Shader compiler tool (需要 Slang SDK，macOS 上可选)
option(BUILD_SHADER_COMPILER "Build the Slang shader compiler tool" OFF)
if(BUILD_SHADER_COMPILER)
    add_subdirectory(tools/shader_compiler)
endif()

# 引擎（暂时直接编译 main.cpp 为可执行文件）
add_subdirectory(engine)

# 编辑器
add_subdirectory(editor)

# ---------------------------------------------------------------------------
# Shader bundle 自动编译（仅当 ShaderCompiler 可用时）
# ---------------------------------------------------------------------------
if(BUILD_SHADER_COMPILER)

set(SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/assets/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/assets/shaders")
set(SHADER_STAMP_FILE "${CMAKE_BINARY_DIR}/generated/shaders/shaderbundles.stamp")

file(GLOB_RECURSE CONFIGURE_DEPENDS SHADER_SLANG_FILES
    "${SHADER_SOURCE_DIR}/*.slang"
)
file(GLOB CONFIGURE_DEPENDS SHADER_COMPILER_FILES
    "${CMAKE_SOURCE_DIR}/tools/shader_compiler/*.cpp"
    "${CMAKE_SOURCE_DIR}/tools/shader_compiler/*.h"
    "${CMAKE_SOURCE_DIR}/tools/shader_compiler/*.hpp"
    "${CMAKE_SOURCE_DIR}/tools/shader_compiler/*.c"
    "${CMAKE_SOURCE_DIR}/tools/shader_compiler/CMakeLists.txt"
)

set(SHADER_BUNDLE_OUTPUTS "")
foreach(shader_file IN LISTS SHADER_SLANG_FILES)
    get_filename_component(shader_name "${shader_file}" NAME_WE)
    list(APPEND SHADER_BUNDLE_OUTPUTS
        "${SHADER_OUTPUT_DIR}/${shader_name}.shaderbundle"
    )
endforeach()

add_custom_command(
    OUTPUT "${SHADER_STAMP_FILE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated/shaders"
    COMMAND $<TARGET_FILE:ShaderCompiler> "${SHADER_SOURCE_DIR}" "${SHADER_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch "${SHADER_STAMP_FILE}"
    DEPENDS
        ShaderCompiler
        ${SHADER_SLANG_FILES}
        ${SHADER_COMPILER_FILES}
    BYPRODUCTS
        ${SHADER_BUNDLE_OUTPUTS}
    COMMENT "Compiling shader bundles"
    VERBATIM
)

add_custom_target(CompileShaders DEPENDS "${SHADER_STAMP_FILE}")

if(TARGET QymEngineLib)
    add_dependencies(QymEngineLib CompileShaders)
endif()

if(TARGET QymEngine)
    add_dependencies(QymEngine CompileShaders)
endif()

if(TARGET QymEditor)
    add_dependencies(QymEditor CompileShaders)
endif()

endif() # BUILD_SHADER_COMPILER
