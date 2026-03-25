# engine/CMakeLists.txt

# 引擎静态库
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scene/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/asset/*.cpp"
)
# D3D11 后端使用 D11_ 前缀结构体，与 D3D12 后端的 VkXxx_T 不冲突
# 两个后端可同时编译

# macOS/Linux: 排除 D3D11/D3D12 后端（仅 Windows）
if(NOT WIN32)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/d3d11/.*")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/d3d12/.*")
endif()

# macOS: Metal 后端 (.mm Objective-C++ 文件)
if(APPLE)
    file(GLOB METAL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/renderer/metal/*.mm")
    # 将 .mm 文件当作 CXX 编译，并添加 ObjC 标志
    set_source_files_properties(${METAL_SOURCES} PROPERTIES LANGUAGE CXX)
    list(APPEND ENGINE_SOURCES ${METAL_SOURCES})
endif()

# GLAD — OpenGL 4.5 Core 函数指针加载器
# 单独编译为静态库，避免 /FI VkDispatch.h (C++ 强制包含) 污染 C 源文件
set(GLAD_SOURCE "${CMAKE_SOURCE_DIR}/3rd-party/glad/src/glad.c")
set(GLAD_INCLUDE "${CMAKE_SOURCE_DIR}/3rd-party/glad/include")

add_library(glad STATIC ${GLAD_SOURCE})
target_include_directories(glad PUBLIC ${GLAD_INCLUDE})
# glad.c 是纯 C 文件，不需要 UTF-8 或 C++ 编译选项

add_library(QymEngineLib STATIC ${ENGINE_SOURCES})
target_include_directories(QymEngineLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(QymEngineLib PUBLIC ${Vulkan_INCLUDE_DIRS})
# GLAD 头文件路径：支持 #include <glad/glad.h> 和 #include <KHR/khrplatform.h>
target_include_directories(QymEngineLib PUBLIC ${GLAD_INCLUDE})
target_link_libraries(QymEngineLib PUBLIC SDL2::SDL2-static glm stb nlohmann_json tinyobjloader glad)
# Windows 上链接 OpenGL32
if(WIN32)
    target_link_libraries(QymEngineLib PUBLIC opengl32)
endif()
# macOS: Metal 框架 + ObjC ARC
if(APPLE)
    set_source_files_properties(${METAL_SOURCES} PROPERTIES
        COMPILE_FLAGS "-fobjc-arc -xobjective-c++"
    )
    target_link_libraries(QymEngineLib PUBLIC "-framework Metal" "-framework QuartzCore")
    # SPIRV-Cross: 运行时 SPIR-V → MSL 转换（Metal 后端用）
    target_include_directories(QymEngineLib PRIVATE "/opt/homebrew/include")
    target_link_libraries(QymEngineLib PRIVATE
        /opt/homebrew/lib/libspirv-cross-msl.a
        /opt/homebrew/lib/libspirv-cross-glsl.a
        /opt/homebrew/lib/libspirv-cross-core.a
        /opt/homebrew/lib/libspirv-cross-cpp.a
    )
endif()
# UTF-8 source encoding
if(MSVC)
    target_compile_options(QymEngineLib PUBLIC /utf-8)
    # VkDispatch.h 自动注入所有编译单元（定义 VK_NO_PROTOTYPES + 函数指针声明）
    target_compile_options(QymEngineLib PUBLIC /FI"renderer/VkDispatch.h")
else()
    # GCC/Clang: -include 等效于 MSVC 的 /FI
    target_compile_options(QymEngineLib PUBLIC -include "${CMAKE_CURRENT_SOURCE_DIR}/renderer/VkDispatch.h")
endif()

target_compile_definitions(QymEngineLib PUBLIC
    $<$<BOOL:${ANDROID}>:VK_USE_PLATFORM_ANDROID_KHR>
    $<$<AND:$<NOT:$<BOOL:${ANDROID}>>,$<BOOL:${WIN32}>>:VK_USE_PLATFORM_WIN32_KHR>
    $<$<BOOL:${APPLE}>:VK_USE_PLATFORM_MACOS_MVK>
    SDL_MAIN_HANDLED
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

# 临时：现有 main.cpp 直接编译为可执行文件
add_executable(QymEngine "${CMAKE_SOURCE_DIR}/QymEngine/main.cpp")
target_link_libraries(QymEngine PRIVATE QymEngineLib)
