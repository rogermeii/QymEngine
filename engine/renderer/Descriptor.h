#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace QymEngine {

class Descriptor {
public:
    void createUboLayout(VkDevice device);      // set 0: UBO only
    void createTextureLayout(VkDevice device);   // set 1: texture only
    void createPool(VkDevice device, int maxFramesInFlight, int maxTextures = 100);
    void createUboSets(VkDevice device, int maxFramesInFlight,
                       const std::vector<VkBuffer>& uniformBuffers);
    VkDescriptorSet createTextureSet(VkDevice device, VkImageView imageView, VkSampler sampler);
    void cleanup(VkDevice device);

    VkDescriptorSetLayout getUboLayout()     const { return m_uboSetLayout; }
    VkDescriptorSetLayout getTextureLayout() const { return m_textureSetLayout; }
    VkDescriptorSet       getUboSet(uint32_t frame) const { return m_uboSets[frame]; }
    VkDescriptorPool      getPool()          const { return m_descriptorPool; }

private:
    VkDescriptorSetLayout        m_uboSetLayout     = VK_NULL_HANDLE;
    VkDescriptorSetLayout        m_textureSetLayout  = VK_NULL_HANDLE;
    VkDescriptorPool             m_descriptorPool    = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_uboSets;
};

} // namespace QymEngine
