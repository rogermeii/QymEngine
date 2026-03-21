#pragma once
#include <string>
#include <map>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace QymEngine {

struct ShaderAsset;

struct MaterialInstance {
    std::string name;
    std::string shaderPath;
    ShaderAsset* shader = nullptr;

    // Set 1 descriptor set (from shader reflection's layout)
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    // MaterialParams UBO
    VkBuffer paramBuffer = VK_NULL_HANDLE;
    VkDeviceMemory paramMemory = VK_NULL_HANDLE;
    void* paramMapped = nullptr;
    uint32_t paramBufferSize = 0;

    // Property values (indexed by reflected member name)
    std::map<std::string, glm::vec4> vec4Props;
    std::map<std::string, float> floatProps;
    std::map<std::string, std::string> texturePaths;
};

// Keep backward compatibility alias
using MaterialAsset = MaterialInstance;

} // namespace QymEngine
