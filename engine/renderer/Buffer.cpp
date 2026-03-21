#include "renderer/Buffer.h"
#include "renderer/VulkanContext.h"
#include <stdexcept>
#include <cstring>

namespace QymEngine {

// --- Static utility ---
void Buffer::createBuffer(VulkanContext& ctx, VkDeviceSize size,
                           VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                           VkBuffer& buffer, VkDeviceMemory& bufferMemory)
{
    VkDevice device = ctx.getDevice();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
        throw std::runtime_error("failed to create buffer!");

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = ctx.findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate buffer memory!");

    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

void Buffer::createUniformBuffers(VulkanContext& ctx, int maxFramesInFlight)
{
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);

    m_uniformBuffers.resize(maxFramesInFlight);
    m_uniformBuffersMemory.resize(maxFramesInFlight);
    m_uniformBuffersMapped.resize(maxFramesInFlight);

    for (int i = 0; i < maxFramesInFlight; i++)
    {
        createBuffer(ctx, bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_uniformBuffers[i], m_uniformBuffersMemory[i]);

        vkMapMemory(ctx.getDevice(), m_uniformBuffersMemory[i], 0, bufferSize, 0, &m_uniformBuffersMapped[i]);
    }
}

void Buffer::cleanup(VkDevice device, int maxFramesInFlight)
{
    for (int i = 0; i < maxFramesInFlight; i++)
    {
        // Unmap before destroy to avoid resource leak
        if (m_uniformBuffersMapped[i]) {
            vkUnmapMemory(device, m_uniformBuffersMemory[i]);
            m_uniformBuffersMapped[i] = nullptr;
        }
        vkDestroyBuffer(device, m_uniformBuffers[i], nullptr);
        vkFreeMemory(device, m_uniformBuffersMemory[i], nullptr);
    }
}

} // namespace QymEngine
