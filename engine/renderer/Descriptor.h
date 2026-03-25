#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace QymEngine {

class Descriptor {
public:
    void createPool(VkDevice device, int maxFramesInFlight, int maxMaterials = 100);

    // Generic set allocation from pool
    VkDescriptorSet allocateSet(VkDevice device, VkDescriptorSetLayout layout);

    // Per-frame UBO sets (set 0)
    void createPerFrameSets(VkDevice device, int maxFramesInFlight,
                            VkDescriptorSetLayout perFrameLayout,
                            const std::vector<VkBuffer>& uniformBuffers);

    VkDescriptorSet getPerFrameSet(uint32_t frame) const { return m_perFrameSets[frame]; }
    VkDescriptorPool getPool() const { return m_descriptorPool; }

    void cleanup(VkDevice device);

private:
    VkDescriptorPool             m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_perFrameSets;
};

} // namespace QymEngine
