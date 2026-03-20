#pragma once
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace QymEngine {

struct ShaderProperty {
    std::string name;
    std::string type;           // "float", "color4", "texture2D"
    glm::vec4 defaultVec = {1,1,1,1};
    float defaultFloat = 0.0f;
    float rangeMin = 0.0f;
    float rangeMax = 1.0f;
    std::string defaultTex;     // "white" or "normal"
};

struct ShaderAsset {
    std::string name;
    std::string vertPath;       // relative to ASSETS_DIR
    std::string fragPath;
    std::vector<ShaderProperty> properties;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

} // namespace QymEngine
