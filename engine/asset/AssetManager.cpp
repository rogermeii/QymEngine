#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <stb_image.h>

#include "asset/AssetManager.h"
#include "renderer/VulkanContext.h"
#include "renderer/CommandManager.h"
#include "renderer/Buffer.h"

#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <algorithm>

namespace fs = std::filesystem;

namespace QymEngine {

void AssetManager::init(VulkanContext& ctx, CommandManager& cmdMgr)
{
    m_ctx = &ctx;
    m_cmdMgr = &cmdMgr;
}

void AssetManager::shutdown(VkDevice device)
{
    for (auto& [key, mesh] : m_meshCache) {
        if (mesh.vertexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mesh.vertexBuffer, nullptr);
        if (mesh.vertexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, mesh.vertexMemory, nullptr);
        if (mesh.indexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mesh.indexBuffer, nullptr);
        if (mesh.indexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, mesh.indexMemory, nullptr);
    }
    m_meshCache.clear();

    for (auto& [key, tex] : m_textureCache) {
        if (tex.sampler != VK_NULL_HANDLE)
            vkDestroySampler(device, tex.sampler, nullptr);
        if (tex.view != VK_NULL_HANDLE)
            vkDestroyImageView(device, tex.view, nullptr);
        if (tex.image != VK_NULL_HANDLE)
            vkDestroyImage(device, tex.image, nullptr);
        if (tex.memory != VK_NULL_HANDLE)
            vkFreeMemory(device, tex.memory, nullptr);
        // descriptorSet is freed when the pool is destroyed
    }
    m_textureCache.clear();
}

void AssetManager::scanAssets(const std::string& assetsDir)
{
    m_assetsDir = assetsDir;
    m_meshFiles.clear();
    m_textureFiles.clear();

    if (!fs::exists(assetsDir)) return;

    for (auto& entry : fs::recursive_directory_iterator(assetsDir)) {
        if (!entry.is_regular_file()) continue;

        // Get relative path from assetsDir
        std::string relPath = fs::relative(entry.path(), assetsDir).generic_string();
        std::string ext = entry.path().extension().string();

        // Convert extension to lowercase
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c){ return std::tolower(c); });

        if (ext == ".obj") {
            m_meshFiles.push_back(relPath);
        } else if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
            m_textureFiles.push_back(relPath);
        }
    }
}

const MeshAsset* AssetManager::loadMesh(const std::string& relativePath)
{
    // Check cache
    auto it = m_meshCache.find(relativePath);
    if (it != m_meshCache.end())
        return &it->second;

    if (!m_ctx || !m_cmdMgr) return nullptr;

    std::string fullPath = m_assetsDir + "/" + relativePath;
    if (!fs::exists(fullPath)) return nullptr;

    // Parse with tinyobjloader
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, fullPath.c_str()))
        return nullptr;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    for (auto& shape : shapes) {
        for (auto& index : shape.mesh.indices) {
            Vertex vertex{};
            vertex.pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };
            vertex.color = {1.0f, 1.0f, 1.0f};
            if (index.texcoord_index >= 0) {
                vertex.texCoord = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1] // flip Y for Vulkan
                };
            }
            if (index.normal_index >= 0) {
                vertex.normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
                };
            }
            indices.push_back(static_cast<uint32_t>(vertices.size()));
            vertices.push_back(vertex);
        }
    }

    if (vertices.empty()) return nullptr;

    MeshAsset mesh{};
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    VkDevice device = m_ctx->getDevice();

    // --- Vertex buffer (staging upload) ---
    {
        VkDeviceSize bufferSize = sizeof(Vertex) * vertices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        Buffer::createBuffer(*m_ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &data);
        memcpy(data, vertices.data(), static_cast<size_t>(bufferSize));
        vkUnmapMemory(device, stagingMemory);

        Buffer::createBuffer(*m_ctx, bufferSize,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             mesh.vertexBuffer, mesh.vertexMemory);

        VkCommandBuffer cmdBuf = m_cmdMgr->beginSingleTimeCommands(device);
        VkBufferCopy copyRegion{};
        copyRegion.size = bufferSize;
        vkCmdCopyBuffer(cmdBuf, stagingBuffer, mesh.vertexBuffer, 1, &copyRegion);
        m_cmdMgr->endSingleTimeCommands(device, m_ctx->getGraphicsQueue(), cmdBuf);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
    }

    // --- Index buffer (staging upload) ---
    {
        VkDeviceSize bufferSize = sizeof(uint32_t) * indices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        Buffer::createBuffer(*m_ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &data);
        memcpy(data, indices.data(), static_cast<size_t>(bufferSize));
        vkUnmapMemory(device, stagingMemory);

        Buffer::createBuffer(*m_ctx, bufferSize,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             mesh.indexBuffer, mesh.indexMemory);

        VkCommandBuffer cmdBuf = m_cmdMgr->beginSingleTimeCommands(device);
        VkBufferCopy copyRegion{};
        copyRegion.size = bufferSize;
        vkCmdCopyBuffer(cmdBuf, stagingBuffer, mesh.indexBuffer, 1, &copyRegion);
        m_cmdMgr->endSingleTimeCommands(device, m_ctx->getGraphicsQueue(), cmdBuf);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
    }

    m_meshCache[relativePath] = mesh;
    return &m_meshCache[relativePath];
}

const TextureAsset* AssetManager::loadTexture(const std::string& relativePath)
{
    // Check cache
    auto it = m_textureCache.find(relativePath);
    if (it != m_textureCache.end())
        return &it->second;

    if (!m_ctx || !m_cmdMgr) return nullptr;

    std::string fullPath = m_assetsDir + "/" + relativePath;
    if (!fs::exists(fullPath)) return nullptr;

    int texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(fullPath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    if (!pixels) return nullptr;

    TextureAsset texAsset{};
    createTextureFromPixels(pixels, texWidth, texHeight, texAsset);
    stbi_image_free(pixels);

    // Create descriptor set for this texture
    if (m_textureSetLayout != VK_NULL_HANDLE && m_textureDescriptorPool != VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_textureDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_textureSetLayout;

        if (vkAllocateDescriptorSets(m_ctx->getDevice(), &allocInfo, &texAsset.descriptorSet) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate texture descriptor set!");

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = texAsset.view;
        imageInfo.sampler = texAsset.sampler;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = texAsset.descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_ctx->getDevice(), 1, &descriptorWrite, 0, nullptr);
    }

    m_textureCache[relativePath] = texAsset;
    return &m_textureCache[relativePath];
}

// ---------------------------------------------------------------------------
// Private helpers (mirroring Texture.cpp patterns)
// ---------------------------------------------------------------------------

void AssetManager::createTextureFromPixels(const unsigned char* pixels, int width, int height,
                                            TextureAsset& outAsset)
{
    VkDevice device = m_ctx->getDevice();
    VkDeviceSize imageSize = width * height * 4;

    // Staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    Buffer::createBuffer(*m_ctx, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         stagingBuffer, stagingMemory);

    void* data;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(device, stagingMemory);

    // Create image
    createImage(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                outAsset.image, outAsset.memory);

    transitionImageLayout(outAsset.image, VK_FORMAT_R8G8B8A8_SRGB,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(stagingBuffer, outAsset.image,
                      static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    transitionImageLayout(outAsset.image, VK_FORMAT_R8G8B8A8_SRGB,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Image view
    outAsset.view = createImageView(outAsset.image, VK_FORMAT_R8G8B8A8_SRGB);

    // Sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_ctx->getPhysicalDevice(), &properties);
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &outAsset.sampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture sampler!");
}

void AssetManager::createImage(uint32_t width, uint32_t height, VkFormat format,
                                VkImageTiling tiling, VkImageUsageFlags usage,
                                VkMemoryPropertyFlags properties,
                                VkImage& image, VkDeviceMemory& imageMemory)
{
    VkDevice device = m_ctx->getDevice();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS)
        throw std::runtime_error("failed to create image!");

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = m_ctx->findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate image memory!");

    vkBindImageMemory(device, image, imageMemory, 0);
}

VkImageView AssetManager::createImageView(VkImage image, VkFormat format)
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    if (vkCreateImageView(m_ctx->getDevice(), &viewInfo, nullptr, &imageView) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture image view!");

    return imageView;
}

void AssetManager::transitionImageLayout(VkImage image, VkFormat format,
                                          VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkDevice device = m_ctx->getDevice();
    VkCommandBuffer commandBuffer = m_cmdMgr->beginSingleTimeCommands(device);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::invalid_argument("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    m_cmdMgr->endSingleTimeCommands(device, m_ctx->getGraphicsQueue(), commandBuffer);
}

void AssetManager::copyBufferToImage(VkBuffer buffer, VkImage image,
                                      uint32_t width, uint32_t height)
{
    VkDevice device = m_ctx->getDevice();
    VkCommandBuffer commandBuffer = m_cmdMgr->beginSingleTimeCommands(device);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(commandBuffer, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    m_cmdMgr->endSingleTimeCommands(device, m_ctx->getGraphicsQueue(), commandBuffer);
}

} // namespace QymEngine
