#pragma once

// ============================================================================
// SPIRV-Cross 封装 — 在纯 C++ 编译单元中调用
//
// VkMetal.mm 使用 -fobjc-arc (ARC) 编译，SPIRV-Cross 在 ARC 模式下
// 析构时会崩溃（ARC 错误地管理 C++ 内部指针）。
// 因此将 SPIRV-Cross 调用提取到纯 .cpp 文件中。
// ============================================================================

#include <vector>
#include <string>
#include <map>
#include <cstdint>

struct MTL_ShaderModule;

/// 将 SPIR-V 二进制通过 SPIRV-Cross 转换为 Metal Shading Language
/// @param spirvData SPIR-V 字数组（uint32_t word array）
/// @param mod 输出：填充 mslSource, entryPointName, executionModel, bindings
void compileSpirvToMSL(std::vector<uint32_t> spirvData, MTL_ShaderModule* mod);
