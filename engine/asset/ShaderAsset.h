#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "renderer/Pipeline.h"

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
    std::string bundlePath;     // relative to ASSETS_DIR (.shaderbundle)
    std::vector<ShaderProperty> properties;
    Pipeline pipeline;          // owns VkPipeline + VkPipelineLayout
};

} // namespace QymEngine
