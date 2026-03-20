#pragma once
#include <string>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace QymEngine {

struct ShaderAsset;

struct MaterialAsset {
    std::string name;
    std::string shaderPath;
    ShaderAsset* shader = nullptr;
    glm::vec4 baseColor = {1, 1, 1, 1};
    float metallic = 0.0f;
    float roughness = 0.5f;
    std::string albedoMapPath;
    std::string normalMapPath;
    VkDescriptorSet textureSet = VK_NULL_HANDLE;
};

} // namespace QymEngine
