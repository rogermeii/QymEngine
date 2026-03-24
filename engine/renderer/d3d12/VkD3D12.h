#pragma once

// ============================================================================
// D3D12 后端 - Vulkan API 封装函数声明
//
// 所有函数以 d3d12_ 前缀命名，签名与 Vulkan API 完全一致。
// VkDispatch.cpp 在 D3D12 模式下将全局函数指针指向这些实现。
// ============================================================================

#ifdef _WIN32

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// 将所有 d3d12_vkXxx 函数指针赋值到全局分发表
void vkLoadD3D12Dispatch();

#endif // _WIN32
