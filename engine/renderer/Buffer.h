#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
#include <vector>

namespace QymEngine {

class VulkanContext;

struct Vertex
{
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;
    glm::vec3 normal;

    static VkVertexInputBindingDescription getBindingDescription()
    {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 4> getAttributeDescriptions()
    {
        std::array<VkVertexInputAttributeDescription, 4> attributeDescriptions{};

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, texCoord);

        attributeDescriptions[3].binding = 0;
        attributeDescriptions[3].location = 3;
        attributeDescriptions[3].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[3].offset = offsetof(Vertex, normal);

        return attributeDescriptions;
    }
};

struct UniformBufferObject
{
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::vec3 lightDir;
    alignas(16) glm::vec3 lightColor;
    alignas(16) glm::vec3 ambientColor;
    alignas(16) glm::vec3 cameraPos;
};

struct PushConstantData
{
    glm::mat4 model;         // 64 bytes
    int highlighted;         // 4 bytes
    uint32_t materialIndex;  // 4 bytes (bindless only, ignored in non-bindless)
    int _pad[2];             // 8 bytes padding → total 80 bytes
};

// Must match the MaterialEntry struct in the bindless shader path exactly
struct BindlessMaterialEntry
{
    glm::vec4 baseColor;     // 16 bytes
    float metallic;          // 4 bytes
    float roughness;         // 4 bytes
    uint32_t albedoTexIndex; // 4 bytes
    uint32_t normalTexIndex; // 4 bytes
    uint32_t samplerIndex;   // 4 bytes
    uint32_t _pad0;          // 4 bytes
    uint32_t _pad1;          // 4 bytes
    uint32_t _pad2;          // 4 bytes
};                           // total: 48 bytes

class Buffer {
public:
    void createUniformBuffers(VulkanContext& ctx, int maxFramesInFlight);
    void cleanup(VkDevice device, int maxFramesInFlight);

    const std::vector<VkBuffer>& getUniformBuffers() const { return m_uniformBuffers; }
    const std::vector<void*>&    getUniformBuffersMapped() const { return m_uniformBuffersMapped; }

    static void createBuffer(VulkanContext& ctx, VkDeviceSize size,
                             VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                             VkBuffer& buffer, VkDeviceMemory& bufferMemory);

private:
    std::vector<VkBuffer>       m_uniformBuffers;
    std::vector<VkDeviceMemory> m_uniformBuffersMemory;
    std::vector<void*>          m_uniformBuffersMapped;
};

} // namespace QymEngine
