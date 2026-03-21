#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <map>
#include "renderer/DescriptorLayoutCache.h"

namespace QymEngine {

struct ReflectedMember {
    std::string name;
    std::string type;
    uint32_t offset;
    uint32_t size;
};

struct ReflectedBinding {
    uint32_t set;
    uint32_t binding;
    std::string type; // "uniformBuffer", "combinedImageSampler", etc.
    std::string name;
    uint32_t size = 0; // UBO size (only for uniformBuffer)
    std::vector<ReflectedMember> members;
    std::vector<std::string> stages;
};

struct ReflectedPushConstant {
    uint32_t offset;
    uint32_t size;
    std::vector<ReflectedMember> members;
    std::vector<std::string> stages;
};

struct ShaderReflectionData {
    std::map<uint32_t, std::vector<ReflectedBinding>> sets;
    std::vector<ReflectedPushConstant> pushConstants;

    // Load from .reflect.json
    bool loadFromJson(const std::string& path);

    // Build Vulkan descriptor set layout bindings for a given set
    std::vector<VkDescriptorSetLayoutBinding> buildBindings(uint32_t setIndex) const;

    // Create push constant ranges from reflection data
    std::vector<VkPushConstantRange> createPushConstantRanges() const;
};

class Pipeline {
public:
    // Create with reflection JSON + layout cache (main path)
    void create(VkDevice device, VkRenderPass renderPass,
                VkExtent2D extent, DescriptorLayoutCache& layoutCache,
                VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
                const std::string& vertPath = "",
                const std::string& fragPath = "");

    // Create with explicit layouts (for special pipelines like grid)
    void createWithLayouts(VkDevice device, VkRenderPass renderPass,
                const std::vector<VkDescriptorSetLayout>& descriptorSetLayouts,
                VkExtent2D extent,
                const std::vector<VkPushConstantRange>& pushConstantRanges = {},
                VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
                const std::string& vertPath = "",
                const std::string& fragPath = "");

    void cleanup(VkDevice device);

    VkPipeline       getPipeline()       const { return m_graphicsPipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }

    const ShaderReflectionData& getReflection() const { return m_reflection; }
    const std::vector<VkDescriptorSetLayout>& getDescriptorSetLayouts() const { return m_setLayouts; }

    static std::vector<char> readFile(const std::string& filename);

private:
    VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code);
    void createPipelineCommon(VkDevice device, VkRenderPass renderPass,
                              VkExtent2D extent, VkPolygonMode polygonMode,
                              VkShaderModule vertModule, VkShaderModule fragModule,
                              const std::vector<VkDescriptorSetLayout>& layouts,
                              const std::vector<VkPushConstantRange>& pcRanges);

    VkPipeline       m_graphicsPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout   = VK_NULL_HANDLE;
    ShaderReflectionData m_reflection;
    std::vector<VkDescriptorSetLayout> m_setLayouts; // cache-managed, not owned
};

} // namespace QymEngine
