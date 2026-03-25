#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace QymEngine {

class VulkanContext;

class CommandManager {
public:
    void createPool(VulkanContext& ctx);
    void createBuffers(VkDevice device, int maxFramesInFlight);
    void cleanup(VkDevice device);

    VkCommandPool   getPool() const { return m_commandPool; }
    VkCommandBuffer getBuffer(uint32_t frame) const { return m_commandBuffers[frame]; }

    VkCommandBuffer beginSingleTimeCommands(VkDevice device);
    void endSingleTimeCommands(VkDevice device, VkQueue graphicsQueue, VkCommandBuffer commandBuffer);

private:
    VkCommandPool                m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
};

} // namespace QymEngine
