# ============================================================================
# QymEngine iOS 构建配置
#
# 生成 Xcode 项目，用于在 iOS 真机上运行编辑器 (Metal 后端)。
#
# 使用方法:
#   mkdir -p ios-build && cd ios-build
#   cmake ../ios-cmake -G Xcode \
#       -DCMAKE_SYSTEM_NAME=iOS \
#       -DCMAKE_OSX_ARCHITECTURES=arm64 \
#       -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
#   open QymEngineIOS.xcodeproj
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(QymEngineIOS LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# iOS 签名配置（真机调试时可通过 CMake 缓存传入团队 ID）
set(QYMENGINE_IOS_DEVELOPMENT_TEAM "" CACHE STRING "iOS 开发团队 ID（可选）")

# iOS 最低部署版本
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
# CMAKE_OSX_SYSROOT 和 CMAKE_OSX_ARCHITECTURES 由 -DCMAKE_SYSTEM_NAME=iOS 自动设置

# 资源路径 — iOS app bundle 中使用相对路径，由 FileUtils.h 通过 SDL_GetBasePath() 解析
add_compile_definitions(ASSETS_DIR="assets")

# ============================================================================
# Vulkan 头文件（仅需要头文件，不链接 Vulkan 库 — Metal 后端替代）
# ============================================================================
# iOS 交叉编译时 find_path 默认只搜索 iOS sysroot，
# 需要 NO_CMAKE_FIND_ROOT_PATH 搜索 macOS 主机路径
find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h
    HINTS
    /opt/homebrew/include
    $ENV{VULKAN_SDK}/include
    $ENV{HOME}/VulkanSDK/latest/macOS/include
    NO_CMAKE_FIND_ROOT_PATH
)
if(NOT VULKAN_INCLUDE_DIR)
    message(FATAL_ERROR "找不到 Vulkan 头文件。请安装 Vulkan SDK 或设置 VULKAN_SDK 环境变量。")
endif()
message(STATUS "Vulkan headers: ${VULKAN_INCLUDE_DIR}")

# ============================================================================
# SDL2 (FetchContent — 支持 iOS 交叉编译)
# ============================================================================
include(FetchContent)
FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.12
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# ============================================================================
# 第三方 header-only 库
# ============================================================================
# GLM
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${CMAKE_SOURCE_DIR}/../3rd-party/glm-1.0.1-light")

# STB
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${CMAKE_SOURCE_DIR}/../3rd-party/stb-master")

# nlohmann/json
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_SOURCE_DIR}/../3rd-party/nlohmann")

# tinyobjloader
add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE "${CMAKE_SOURCE_DIR}/../3rd-party/tinyobjloader")

# ============================================================================
# ImGui (docking 分支) + ImGuizmo
# ============================================================================
file(GLOB IMGUI_SOURCES
    "${CMAKE_SOURCE_DIR}/../3rd-party/imgui/*.cpp"
    "${CMAKE_SOURCE_DIR}/../3rd-party/imgui/backends/imgui_impl_sdl2.cpp"
    "${CMAKE_SOURCE_DIR}/../3rd-party/imgui/backends/imgui_impl_vulkan.cpp"
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    "${CMAKE_SOURCE_DIR}/../3rd-party/imgui"
    "${CMAKE_SOURCE_DIR}/../3rd-party/imgui/backends"
)
target_include_directories(imgui PUBLIC ${VULKAN_INCLUDE_DIR})
target_link_libraries(imgui PUBLIC SDL2::SDL2-static)
target_compile_definitions(imgui PUBLIC VK_NO_PROTOTYPES)

# ImGuizmo (作为 imgui 库的一部分编译)
target_sources(imgui PRIVATE "${CMAKE_SOURCE_DIR}/../3rd-party/imguizmo/ImGuizmo.cpp")
target_include_directories(imgui PUBLIC "${CMAKE_SOURCE_DIR}/../3rd-party/imguizmo")

# ============================================================================
# 引擎源文件
# ============================================================================
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_SOURCE_DIR}/../engine/core/*.cpp"
    "${CMAKE_SOURCE_DIR}/../engine/renderer/*.cpp"
    "${CMAKE_SOURCE_DIR}/../engine/scene/*.cpp"
    "${CMAKE_SOURCE_DIR}/../engine/asset/*.cpp"
)
# 排除非 iOS 后端
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/d3d11/.*")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/d3d12/.*")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/opengl/.*")

# Metal 后端 (.mm Objective-C++ 文件)
file(GLOB METAL_SOURCES "${CMAKE_SOURCE_DIR}/../engine/renderer/metal/*.mm")
list(APPEND ENGINE_SOURCES ${METAL_SOURCES})

# ============================================================================
# 编辑器源文件 (排除 editor/main.cpp，使用 ios-cmake/main.mm)
# ============================================================================
file(GLOB_RECURSE EDITOR_SOURCES
    "${CMAKE_SOURCE_DIR}/../editor/*.cpp"
)
# 排除编辑器原有的 main.cpp（使用 iOS 专用入口）
list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/editor/main\\.cpp$")

# iOS 入口文件
set(IOS_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/main.mm")

# ============================================================================
# App bundle 资源文件
# ============================================================================
file(GLOB_RECURSE ASSET_FILES
    "${CMAKE_SOURCE_DIR}/../assets/*"
)

# LaunchScreen.storyboard
set(LAUNCH_STORYBOARD "${CMAKE_CURRENT_SOURCE_DIR}/LaunchScreen.storyboard")

# ============================================================================
# 可执行文件 (iOS app bundle)
# ============================================================================
add_executable(QymEditorIOS MACOSX_BUNDLE
    ${IOS_MAIN}
    ${ENGINE_SOURCES}
    ${EDITOR_SOURCES}
    ${ASSET_FILES}
    ${LAUNCH_STORYBOARD}
)

# 资源文件设置为 bundle resource，保持目录结构
foreach(asset ${ASSET_FILES})
    file(RELATIVE_PATH asset_rel "${CMAKE_SOURCE_DIR}/../assets" "${asset}")
    get_filename_component(asset_dir "${asset_rel}" DIRECTORY)
    set_source_files_properties("${asset}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/assets/${asset_dir}"
    )
endforeach()

# LaunchScreen.storyboard 放入 bundle 根目录
set_source_files_properties("${LAUNCH_STORYBOARD}" PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

# Metal .mm 文件和 iOS 入口文件的编译选项
set_source_files_properties(${METAL_SOURCES} ${IOS_MAIN} PROPERTIES
    COMPILE_FLAGS "-fobjc-arc -xobjective-c++"
)

# ============================================================================
# 包含路径
# ============================================================================
target_include_directories(QymEditorIOS PRIVATE
    "${CMAKE_SOURCE_DIR}/../engine"
    "${CMAKE_SOURCE_DIR}/../editor"
    ${VULKAN_INCLUDE_DIR}
)

# GLAD 头文件路径（OpenGL 后端已排除，但部分头文件可能间接引用）
target_include_directories(QymEditorIOS PRIVATE
    "${CMAKE_SOURCE_DIR}/../3rd-party/glad/include"
)

# ============================================================================
# 链接库
# ============================================================================
target_link_libraries(QymEditorIOS PRIVATE
    SDL2::SDL2-static SDL2::SDL2main
    glm stb nlohmann_json tinyobjloader imgui
    "-framework Metal"
    "-framework QuartzCore"
    "-framework UIKit"
    "-framework Foundation"
    "-framework CoreGraphics"
    "-framework AVFoundation"
    "-framework AudioToolbox"
    "-framework CoreMotion"
    "-framework GameController"
    "-framework CoreHaptics"
    "-framework CoreBluetooth"
)

# ============================================================================
# 编译定义
# ============================================================================
target_compile_definitions(QymEditorIOS PRIVATE
    VK_NO_PROTOTYPES
    SDL_MAIN_HANDLED
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

# VkDispatch.h 强制包含所有编译单元（与 engine/CMakeLists.txt 保持一致）
target_compile_options(QymEditorIOS PRIVATE
    -include "${CMAKE_SOURCE_DIR}/../engine/renderer/VkDispatch.h"
)

# ============================================================================
# iOS app bundle 配置
# ============================================================================
set_target_properties(QymEditorIOS PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.qymengine.editor"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"  # iPhone + iPad
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
)

if(QYMENGINE_IOS_DEVELOPMENT_TEAM)
    set_target_properties(QymEditorIOS PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${QYMENGINE_IOS_DEVELOPMENT_TEAM}"
    )
endif()
