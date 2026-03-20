#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace QymEngine {

class VulkanContext;
class CommandManager;

struct MeshAsset {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
};

struct TextureAsset {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

class AssetManager {
public:
    void init(VulkanContext& ctx, CommandManager& cmdMgr);
    void shutdown(VkDevice device);

    void scanAssets(const std::string& assetsDir);

    const MeshAsset* loadMesh(const std::string& relativePath);
    const TextureAsset* loadTexture(const std::string& relativePath);

    const std::vector<std::string>& getMeshFiles() const { return m_meshFiles; }
    const std::vector<std::string>& getTextureFiles() const { return m_textureFiles; }

    // For per-texture descriptor set creation
    void setTextureDescriptorSetLayout(VkDescriptorSetLayout layout) { m_textureSetLayout = layout; }
    void setTextureDescriptorPool(VkDescriptorPool pool) { m_textureDescriptorPool = pool; }

private:
    VulkanContext* m_ctx = nullptr;
    CommandManager* m_cmdMgr = nullptr;
    std::string m_assetsDir;

    std::vector<std::string> m_meshFiles;    // relative paths like "models/cube.obj"
    std::vector<std::string> m_textureFiles; // relative paths like "textures/wall.jpg"

    std::unordered_map<std::string, MeshAsset> m_meshCache;
    std::unordered_map<std::string, TextureAsset> m_textureCache;

    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_textureDescriptorPool = VK_NULL_HANDLE;

    // Helper: create VkImage + memory + view + sampler from pixel data
    void createTextureFromPixels(const unsigned char* pixels, int width, int height,
                                 TextureAsset& outAsset);
    void createImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags properties,
                     VkImage& image, VkDeviceMemory& imageMemory);
    VkImageView createImageView(VkImage image, VkFormat format);
    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout);
    void copyBufferToImage(VkBuffer buffer, VkImage image,
                           uint32_t width, uint32_t height);
};

} // namespace QymEngine
