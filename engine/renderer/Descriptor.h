#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace QymEngine {

class Descriptor {
public:
    void createLayout(VkDevice device);
    void createPool(VkDevice device, int maxFramesInFlight);
    void createSets(VkDevice device, int maxFramesInFlight,
                    const std::vector<VkBuffer>& uniformBuffers,
                    VkImageView textureImageView, VkSampler textureSampler);
    void cleanup(VkDevice device);

    VkDescriptorSetLayout getLayout() const { return m_descriptorSetLayout; }
    VkDescriptorSet       getSet(uint32_t frame) const { return m_descriptorSets[frame]; }

private:
    VkDescriptorSetLayout        m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool             m_descriptorPool      = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
};

} // namespace QymEngine
