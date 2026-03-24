#include "renderer/Descriptor.h"
#include "renderer/VkDispatch.h"
#include "renderer/Buffer.h"
#include <array>
#include <stdexcept>

namespace QymEngine {

void Descriptor::createPool(VkDevice device, int maxFramesInFlight, int maxMaterials)
{
    // Extra margin for editor/preview descriptor sets
    const int editorExtra = 10;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    // UBOs: per-frame sets + per-material param UBOs + editor reserve
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = static_cast<uint32_t>(maxFramesInFlight + maxMaterials + editorExtra);
    // Samplers: each material can have up to 4 texture slots + editor reserve
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(maxMaterials * 4 + editorExtra);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = static_cast<uint32_t>(maxFramesInFlight + maxMaterials + editorExtra);

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create descriptor pool!");
}

VkDescriptorSet Descriptor::allocateSet(VkDevice device, VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet descriptorSet;
    if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate descriptor set!");

    return descriptorSet;
}

void Descriptor::createPerFrameSets(VkDevice device, int maxFramesInFlight,
                                     VkDescriptorSetLayout perFrameLayout,
                                     const std::vector<VkBuffer>& uniformBuffers)
{
    std::vector<VkDescriptorSetLayout> layouts(maxFramesInFlight, perFrameLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(maxFramesInFlight);
    allocInfo.pSetLayouts = layouts.data();

    m_perFrameSets.resize(maxFramesInFlight);
    if (vkAllocateDescriptorSets(device, &allocInfo, m_perFrameSets.data()) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate per-frame descriptor sets!");

    for (int i = 0; i < maxFramesInFlight; i++)
    {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_perFrameSets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }
}

void Descriptor::cleanup(VkDevice device)
{
    vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;
}

} // namespace QymEngine
