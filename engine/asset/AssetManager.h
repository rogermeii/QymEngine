#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "asset/ShaderAsset.h"
#include "asset/MaterialAsset.h"

namespace QymEngine {

class VulkanContext;
class CommandManager;

struct MeshAsset {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    // Bounding box
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
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
    const ShaderAsset* loadShader(const std::string& relativePath);
    const MaterialAsset* loadMaterial(const std::string& relativePath);

    const std::vector<std::string>& getMeshFiles() const { return m_meshFiles; }
    const std::vector<std::string>& getTextureFiles() const { return m_textureFiles; }
    const std::vector<std::string>& getShaderFiles() const { return m_shaderFiles; }
    const std::vector<std::string>& getMaterialFiles() const { return m_materialFiles; }

    // For fallback textures (called by Renderer after creating fallbacks)
    void setFallbackAlbedo(VkImageView view, VkSampler sampler) { m_fallbackAlbedoView = view; m_fallbackAlbedoSampler = sampler; }
    void setFallbackNormal(VkImageView view, VkSampler sampler) { m_fallbackNormalView = view; m_fallbackNormalSampler = sampler; }

    // For per-texture descriptor set creation
    void setTextureDescriptorSetLayout(VkDescriptorSetLayout layout) { m_textureSetLayout = layout; }
    void setTextureDescriptorPool(VkDescriptorPool pool) { m_textureDescriptorPool = pool; }

    // For shader pipeline creation - called by Renderer after offscreen setup
    void setOffscreenRenderPass(VkRenderPass rp) { m_offscreenRenderPass = rp; }
    void setOffscreenExtent(VkExtent2D ext) { m_offscreenExtent = ext; }
    void setDescriptorSetLayouts(VkDescriptorSetLayout ubo, VkDescriptorSetLayout tex) {
        m_uboLayout = ubo; m_texLayout = tex;
    }

private:
    VulkanContext* m_ctx = nullptr;
    CommandManager* m_cmdMgr = nullptr;
    std::string m_assetsDir;

    std::vector<std::string> m_meshFiles;      // relative paths like "models/cube.obj"
    std::vector<std::string> m_textureFiles;   // relative paths like "textures/wall.jpg"
    std::vector<std::string> m_shaderFiles;    // relative paths like "shaders/standard_lit.shader.json"
    std::vector<std::string> m_materialFiles;  // relative paths like "materials/default_lit.mat.json"

    std::unordered_map<std::string, MeshAsset> m_meshCache;
    std::unordered_map<std::string, TextureAsset> m_textureCache;
    std::unordered_map<std::string, ShaderAsset> m_shaderCache;
    std::unordered_map<std::string, MaterialAsset> m_materialCache;

    // Fallback textures for materials without explicit maps
    VkImageView m_fallbackAlbedoView = VK_NULL_HANDLE;
    VkSampler m_fallbackAlbedoSampler = VK_NULL_HANDLE;
    VkImageView m_fallbackNormalView = VK_NULL_HANDLE;
    VkSampler m_fallbackNormalSampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_textureDescriptorPool = VK_NULL_HANDLE;

    // For shader pipeline creation
    VkRenderPass m_offscreenRenderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_uboLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_texLayout = VK_NULL_HANDLE;
    VkExtent2D m_offscreenExtent = {0, 0};

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
