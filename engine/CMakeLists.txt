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
# UTF-8 source encoding
if(MSVC)
    target_compile_options(QymEngineLib PUBLIC /utf-8)
    # VkDispatch.h 自动注入所有编译单元（定义 VK_NO_PROTOTYPES + 函数指针声明）
    target_compile_options(QymEngineLib PUBLIC /FI"renderer/VkDispatch.h")
endif()

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
