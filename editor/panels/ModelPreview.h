#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>

namespace QymEngine {

class Renderer;
enum class MeshType;
struct MeshAsset;

class ModelPreview {
public:
    void init(Renderer& renderer);
    void shutdown(VkDevice device);

    void renderBuiltIn(Renderer& renderer, MeshType type);
    void renderMesh(Renderer& renderer, const MeshAsset* mesh);

    VkDescriptorSet getDescriptorSet() const { return m_descriptorSet; }
    bool isReady() const { return m_framebuffer != VK_NULL_HANDLE; }

private:
    void createResources(Renderer& renderer);
    void writePreviewUbo(const glm::vec3& boundsMin, const glm::vec3& boundsMax);
    void beginPreviewPass(Renderer& renderer);

    static constexpr uint32_t PREVIEW_SIZE = 256;

    VkImage m_colorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_colorMemory = VK_NULL_HANDLE;
    VkImageView m_colorView = VK_NULL_HANDLE;

    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;

    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;

    // Dedicated UBO for preview camera (avoids timing issue with shared UBO)
    VkBuffer m_uboBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_uboMemory = VK_NULL_HANDLE;
    void* m_uboMapped = nullptr;
    VkDescriptorSet m_uboDescriptorSet = VK_NULL_HANDLE;

    bool m_initialized = false;
};

} // namespace QymEngine
