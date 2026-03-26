cmake_minimum_required(VERSION 3.20)
project(ShaderCompiler LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

# SPIRV-Reflect (build-time only, not linked into engine)
add_library(spirv_reflect_tool STATIC
    "${CMAKE_SOURCE_DIR}/3rd-party/spirv-reflect/spirv_reflect.c"
)
target_include_directories(spirv_reflect_tool PUBLIC
    "${CMAKE_SOURCE_DIR}/3rd-party/spirv-reflect"
    "${CMAKE_SOURCE_DIR}/3rd-party/spirv-reflect/include"
)

add_executable(ShaderCompiler main.cpp)

# UTF-8 source encoding (中文注释)
if(MSVC)
    target_compile_options(ShaderCompiler PRIVATE /utf-8)
endif()

# Slang SDK: 优先使用 SLANG_SDK_PATH，其次从 Vulkan SDK 查找
if(DEFINED SLANG_SDK_PATH AND EXISTS "${SLANG_SDK_PATH}/include/slang.h")
    message(STATUS "Using Slang SDK from: ${SLANG_SDK_PATH}")
    target_include_directories(ShaderCompiler PRIVATE "${SLANG_SDK_PATH}/include")
    target_link_directories(ShaderCompiler PRIVATE "${SLANG_SDK_PATH}/lib")
    if(APPLE)
        target_link_libraries(ShaderCompiler PRIVATE "${SLANG_SDK_PATH}/lib/libslang.dylib")
    else()
        target_link_libraries(ShaderCompiler PRIVATE slang)
    endif()
else()
    # Fallback: Vulkan SDK 内置 Slang (Windows)
    target_include_directories(ShaderCompiler PRIVATE "${Vulkan_INCLUDE_DIRS}/../Include/slang")
    target_link_directories(ShaderCompiler PRIVATE "${Vulkan_INCLUDE_DIRS}/../Lib")
    target_link_libraries(ShaderCompiler PRIVATE slang)
endif()
target_link_libraries(ShaderCompiler PRIVATE spirv_reflect_tool)

# nlohmann/json for output
target_include_directories(ShaderCompiler PRIVATE "${CMAKE_SOURCE_DIR}/3rd-party/nlohmann")
