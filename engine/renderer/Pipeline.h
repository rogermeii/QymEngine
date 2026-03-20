#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace QymEngine {

class Pipeline {
public:
    void create(VkDevice device, VkRenderPass renderPass,
                const std::vector<VkDescriptorSetLayout>& descriptorSetLayouts,
                VkExtent2D extent,
                const std::vector<VkPushConstantRange>& pushConstantRanges = {},
                VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL);
    void cleanup(VkDevice device);

    VkPipeline       getPipeline()       const { return m_graphicsPipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }

private:
    static std::vector<char> readFile(const std::string& filename);
    VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code);

    VkPipeline       m_graphicsPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout   = VK_NULL_HANDLE;
};

} // namespace QymEngine
