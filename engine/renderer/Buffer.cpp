#include "renderer/Buffer.h"
#include "renderer/VulkanContext.h"
#include "renderer/CommandManager.h"
#include <stdexcept>
#include <cstring>

namespace QymEngine {

const std::vector<Vertex> Buffer::s_vertices = {
    {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f,  0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}
};

const std::vector<uint32_t> Buffer::s_indices = {
    0, 1, 2, 2, 3, 0
};

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

// --- Private ---
void Buffer::copyBuffer(VulkanContext& ctx, CommandManager& cmdMgr,
                         VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
    VkCommandBuffer commandBuffer = cmdMgr.beginSingleTimeCommands(ctx.getDevice());

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    cmdMgr.endSingleTimeCommands(ctx.getDevice(), ctx.getGraphicsQueue(), commandBuffer);
}

// --- Public ---
void Buffer::createVertexBuffer(VulkanContext& ctx, CommandManager& cmdMgr)
{
    VkDeviceSize bufferSize = sizeof(s_vertices[0]) * s_vertices.size();

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    createBuffer(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingBufferMemory);

    void* data;
    vkMapMemory(ctx.getDevice(), stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, s_vertices.data(), (size_t)bufferSize);
    vkUnmapMemory(ctx.getDevice(), stagingBufferMemory);

    createBuffer(ctx, bufferSize,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 m_vertexBuffer, m_vertexBufferMemory);

    copyBuffer(ctx, cmdMgr, stagingBuffer, m_vertexBuffer, bufferSize);

    vkDestroyBuffer(ctx.getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(ctx.getDevice(), stagingBufferMemory, nullptr);
}

void Buffer::createIndexBuffer(VulkanContext& ctx, CommandManager& cmdMgr)
{
    VkDeviceSize bufferSize = sizeof(s_indices[0]) * s_indices.size();

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    createBuffer(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingBufferMemory);

    void* data;
    vkMapMemory(ctx.getDevice(), stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, s_indices.data(), (size_t)bufferSize);
    vkUnmapMemory(ctx.getDevice(), stagingBufferMemory);

    createBuffer(ctx, bufferSize,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 m_indexBuffer, m_indexBufferMemory);

    copyBuffer(ctx, cmdMgr, stagingBuffer, m_indexBuffer, bufferSize);

    vkDestroyBuffer(ctx.getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(ctx.getDevice(), stagingBufferMemory, nullptr);
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
        vkDestroyBuffer(device, m_uniformBuffers[i], nullptr);
        vkFreeMemory(device, m_uniformBuffersMemory[i], nullptr);
    }

    vkDestroyBuffer(device, m_indexBuffer, nullptr);
    vkFreeMemory(device, m_indexBufferMemory, nullptr);

    vkDestroyBuffer(device, m_vertexBuffer, nullptr);
    vkFreeMemory(device, m_vertexBufferMemory, nullptr);
}

} // namespace QymEngine
