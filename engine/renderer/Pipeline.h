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

    // Load from JSON string
    bool loadFromString(const std::string& jsonStr);

    // Build Vulkan descriptor set layout bindings for a given set
    std::vector<VkDescriptorSetLayoutBinding> buildBindings(uint32_t setIndex) const;

    // Create push constant ranges from reflection data
    std::vector<VkPushConstantRange> createPushConstantRanges() const;
};

class Pipeline {
public:
    // Create from in-memory SPIR-V + reflection JSON (ShaderBundle)
    void createFromMemory(VkDevice device, VkRenderPass renderPass,
                VkExtent2D extent, DescriptorLayoutCache& layoutCache,
                VkPolygonMode polygonMode,
                const std::vector<char>& vertSpv,
                const std::vector<char>& fragSpv,
                const std::string& reflectJson);

    // Create with explicit layouts and in-memory SPIR-V (bindless/grid)
    void createWithLayoutsFromMemory(VkDevice device, VkRenderPass renderPass,
                const std::vector<VkDescriptorSetLayout>& descriptorSetLayouts,
                VkExtent2D extent,
                const std::vector<VkPushConstantRange>& pushConstantRanges,
                VkPolygonMode polygonMode,
                const std::vector<char>& vertSpv,
                const std::vector<char>& fragSpv,
                const std::string& reflectJson = "");

    void cleanup(VkDevice device);

    VkPipeline       getPipeline()       const { return m_graphicsPipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }

    const ShaderReflectionData& getReflection() const { return m_reflection; }
    const std::vector<VkDescriptorSetLayout>& getDescriptorSetLayouts() const { return m_setLayouts; }

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
