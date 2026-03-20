#pragma once
#include <vulkan/vulkan.h>
#include <string>

namespace QymEngine {

class VulkanContext;
class CommandManager;

class Texture {
public:
    void createTextureImage(VulkanContext& ctx, CommandManager& cmdMgr);
    void createTextureImageView(VkDevice device);
    void createTextureSampler(VulkanContext& ctx);
    void cleanup(VkDevice device);

    VkImageView getImageView() const { return m_textureImageView; }
    VkSampler   getSampler()   const { return m_textureSampler; }

private:
    void createImage(VulkanContext& ctx, uint32_t width, uint32_t height,
                     VkFormat format, VkImageTiling tiling,
                     VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                     VkImage& image, VkDeviceMemory& imageMemory);

    VkImageView createImageView(VkDevice device, VkImage image, VkFormat format);

    void transitionImageLayout(VulkanContext& ctx, CommandManager& cmdMgr,
                               VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout);

    void copyBufferToImage(VulkanContext& ctx, CommandManager& cmdMgr,
                           VkBuffer buffer, VkImage image,
                           uint32_t width, uint32_t height);

    VkImage        m_textureImage       = VK_NULL_HANDLE;
    VkDeviceMemory m_textureImageMemory = VK_NULL_HANDLE;
    VkImageView    m_textureImageView   = VK_NULL_HANDLE;
    VkSampler      m_textureSampler     = VK_NULL_HANDLE;
};

} // namespace QymEngine
