#include "renderer/Renderer.h"
#include "core/Window.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <fstream>
#include <stdexcept>
#include <cstring>

namespace QymEngine {

void Renderer::init(Window& window)
{
    m_window = &window;

    // Register resize callback
    m_window->setFramebufferResizeCallback([this](int, int) {
        m_framebufferResized = true;
    });

    GLFWwindow* nativeWindow = m_window->getNativeWindow();

    // Init order follows dependency chain
    m_context.init(nativeWindow);
    m_swapChain.create(m_context, nativeWindow);
    m_commandManager.createPool(m_context);

    // The original scene RenderPass (used for swapchain). Kept for init compatibility.
    m_renderPass.create(m_context.getDevice(), m_swapChain.getImageFormat());

    // Create split descriptor set layouts: set 0 = UBO, set 1 = texture
    m_descriptor.createUboLayout(m_context.getDevice());
    m_descriptor.createTextureLayout(m_context.getDevice());

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstantData);
    std::vector<VkPushConstantRange> pushConstantRanges = { pushConstantRange };

    // Pipeline layout now uses both descriptor set layouts
    std::vector<VkDescriptorSetLayout> setLayouts = {
        m_descriptor.getUboLayout(),
        m_descriptor.getTextureLayout()
    };
    m_pipeline.create(m_context.getDevice(), m_renderPass.get(),
                      setLayouts, m_swapChain.getExtent(),
                      pushConstantRanges);

    // Swapchain framebuffers
    m_swapChain.createFramebuffers(m_context.getDevice(), m_renderPass.get());

    m_texture.createTextureImage(m_context, m_commandManager);
    m_texture.createTextureImageView(m_context.getDevice());
    m_texture.createTextureSampler(m_context);

    m_meshLibrary.init(m_context, m_commandManager);
    m_buffer.createUniformBuffers(m_context, MAX_FRAMES_IN_FLIGHT);

    // Create descriptor pool with space for UBO sets + texture sets
    m_descriptor.createPool(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT, 100);

    // Create UBO descriptor sets (set 0, per frame-in-flight)
    m_descriptor.createUboSets(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT,
                               m_buffer.getUniformBuffers());

    // Create default texture descriptor set (set 1) for the existing texture.jpg
    m_defaultTextureSet = m_descriptor.createTextureSet(
        m_context.getDevice(), m_texture.getImageView(), m_texture.getSampler());

    // Create fallback textures for the material system
    createFallbackTextures();

    // Initialize AssetManager
    m_assetManager.init(m_context, m_commandManager);
    m_assetManager.setTextureDescriptorSetLayout(m_descriptor.getTextureLayout());
    m_assetManager.setTextureDescriptorPool(m_descriptor.getPool());
    m_assetManager.setFallbackAlbedo(m_whiteFallbackView, m_fallbackSampler);
    m_assetManager.setFallbackNormal(m_normalFallbackView, m_fallbackSampler);
    m_assetManager.scanAssets(std::string(ASSETS_DIR));

    m_commandManager.createBuffers(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT);
    m_swapChain.createSyncObjects(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT);
}

bool Renderer::beginFrame()
{
    VkDevice device = m_context.getDevice();

    VkFence inFlightFence = m_swapChain.getInFlightFence(m_currentFrame);
    vkWaitForFences(device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);

    VkResult result = vkAcquireNextImageKHR(device, m_swapChain.getSwapChain(), UINT64_MAX,
                                             m_swapChain.getImageAvailableSemaphore(m_currentFrame),
                                             VK_NULL_HANDLE, &m_currentImageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        recreateSwapChain();
        return false;
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    updateUniformBuffer(m_currentFrame);

    vkResetFences(device, 1, &inFlightFence);

    // Begin command buffer here (used by both offscreen + ImGui passes)
    VkCommandBuffer cmdBuf = m_commandManager.getBuffer(m_currentFrame);
    vkResetCommandBuffer(cmdBuf, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags            = 0;
    beginInfo.pInheritanceInfo = nullptr;

    if (vkBeginCommandBuffer(cmdBuf, &beginInfo) != VK_SUCCESS)
        throw std::runtime_error("failed to begin recording command buffer!");

    return true;
}

void Renderer::drawScene(Scene& scene)
{
    m_activeScene = &scene;
    if (m_offscreenRenderPass != VK_NULL_HANDLE && m_offscreenFramebuffer != VK_NULL_HANDLE)
    {
        VkCommandBuffer cmdBuf = m_commandManager.getBuffer(m_currentFrame);
        drawSceneToOffscreen(cmdBuf, scene);
    }
}

void Renderer::endFrame()
{
    VkDevice device = m_context.getDevice();
    VkCommandBuffer cmdBuf = m_commandManager.getBuffer(m_currentFrame);

    if (vkEndCommandBuffer(cmdBuf) != VK_SUCCESS)
        throw std::runtime_error("failed to record command buffer!");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = { m_swapChain.getImageAvailableSemaphore(m_currentFrame) };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores    = waitSemaphores;
    submitInfo.pWaitDstStageMask  = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmdBuf;

    VkSemaphore signalSemaphores[] = { m_swapChain.getRenderFinishedSemaphore(m_currentFrame) };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = signalSemaphores;

    if (vkQueueSubmit(m_context.getGraphicsQueue(), 1, &submitInfo,
                      m_swapChain.getInFlightFence(m_currentFrame)) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = signalSemaphores;

    VkSwapchainKHR swapChains[] = { m_swapChain.getSwapChain() };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains    = swapChains;
    presentInfo.pImageIndices  = &m_currentImageIndex;
    presentInfo.pResults       = nullptr;

    VkResult result = vkQueuePresentKHR(m_context.getPresentQueue(), &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized)
    {
        m_framebufferResized = false;
        recreateSwapChain();
    }
    else if (result != VK_SUCCESS)
    {
        throw std::runtime_error("failed to present swap chain image!");
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Renderer::createFallbackTextures()
{
    VkDevice device = m_context.getDevice();

    // Helper lambda: create a 1x1 texture
    auto createFallback = [&](const unsigned char pixel[4], VkFormat format,
                              VkImage& outImage, VkDeviceMemory& outMemory, VkImageView& outView)
    {
        VkDeviceSize imageSize = 4; // 1x1 RGBA

        // Staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        Buffer::createBuffer(m_context, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
        memcpy(data, pixel, 4);
        vkUnmapMemory(device, stagingMemory);

        // Create image
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {1, 1, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &imageInfo, nullptr, &outImage) != VK_SUCCESS)
            throw std::runtime_error("failed to create fallback image!");

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, outImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = m_context.findMemoryType(
            memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate fallback image memory!");

        vkBindImageMemory(device, outImage, outMemory, 0);

        // Transition + copy
        VkCommandBuffer cmd = m_commandManager.beginSingleTimeCommands(device);

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = outImage;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(cmd, stagingBuffer, outImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        m_commandManager.endSingleTimeCommands(device, m_context.getGraphicsQueue(), cmd);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);

        // Image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = outImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(device, &viewInfo, nullptr, &outView) != VK_SUCCESS)
            throw std::runtime_error("failed to create fallback image view!");
    };

    // White 1x1 (albedo fallback)
    const unsigned char whitePixel[4] = {255, 255, 255, 255};
    createFallback(whitePixel, VK_FORMAT_R8G8B8A8_SRGB,
        m_whiteFallbackImage, m_whiteFallbackMemory, m_whiteFallbackView);

    // Normal 1x1 (flat normal pointing up: 0.5, 0.5, 1.0 in tangent space)
    const unsigned char normalPixel[4] = {128, 128, 255, 255};
    createFallback(normalPixel, VK_FORMAT_R8G8B8A8_UNORM,
        m_normalFallbackImage, m_normalFallbackMemory, m_normalFallbackView);

    // Shared sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_fallbackSampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create fallback sampler!");

    // Default material texture set (albedo=white, normal=flat)
    m_defaultMaterialTexSet = m_descriptor.createTextureSet(device,
        m_whiteFallbackView, m_fallbackSampler,
        m_normalFallbackView, m_fallbackSampler);
}

void Renderer::shutdown()
{
    vkDeviceWaitIdle(m_context.getDevice());

    VkDevice device = m_context.getDevice();

    destroyOffscreen();

    // Cleanup fallback textures
    if (m_fallbackSampler != VK_NULL_HANDLE)
        vkDestroySampler(device, m_fallbackSampler, nullptr);
    if (m_whiteFallbackView != VK_NULL_HANDLE)
        vkDestroyImageView(device, m_whiteFallbackView, nullptr);
    if (m_whiteFallbackImage != VK_NULL_HANDLE)
        vkDestroyImage(device, m_whiteFallbackImage, nullptr);
    if (m_whiteFallbackMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, m_whiteFallbackMemory, nullptr);
    if (m_normalFallbackView != VK_NULL_HANDLE)
        vkDestroyImageView(device, m_normalFallbackView, nullptr);
    if (m_normalFallbackImage != VK_NULL_HANDLE)
        vkDestroyImage(device, m_normalFallbackImage, nullptr);
    if (m_normalFallbackMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, m_normalFallbackMemory, nullptr);

    m_assetManager.shutdown(device);
    m_swapChain.cleanup(device);
    m_texture.cleanup(device);
    m_meshLibrary.shutdown(device);
    m_buffer.cleanup(device, MAX_FRAMES_IN_FLIGHT);
    m_descriptor.cleanup(device);
    if (m_gridPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_gridPipeline, nullptr);
        m_gridPipeline = VK_NULL_HANDLE;
    }
    if (m_gridPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_gridPipelineLayout, nullptr);
        m_gridPipelineLayout = VK_NULL_HANDLE;
    }
    m_offscreenPipeline.cleanup(device);
    m_pipeline.cleanup(device);
    m_renderPass.cleanup(device);
    m_swapChain.cleanupSyncObjects(device, MAX_FRAMES_IN_FLIGHT);
    m_commandManager.cleanup(device);
    m_context.shutdown();
}

// ---------------------------------------------------------------------------
// Offscreen rendering
// ---------------------------------------------------------------------------

void Renderer::createOffscreen(uint32_t width, uint32_t height)
{
    VkDevice device = m_context.getDevice();
    VkFormat format = m_swapChain.getImageFormat();

    m_offscreenWidth  = width;
    m_offscreenHeight = height;

    // --- 1. Create offscreen image ---
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width  = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth  = 1;
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.format        = format;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &m_offscreenImage) != VK_SUCCESS)
        throw std::runtime_error("failed to create offscreen image!");

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, m_offscreenImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memRequirements.size;
    allocInfo.memoryTypeIndex = m_context.findMemoryType(memRequirements.memoryTypeBits,
                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &m_offscreenMemory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate offscreen image memory!");

    vkBindImageMemory(device, m_offscreenImage, m_offscreenMemory, 0);

    // --- 2. Create color image view ---
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_offscreenImage;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = format;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_offscreenImageView) != VK_SUCCESS)
        throw std::runtime_error("failed to create offscreen image view!");

    // --- 2b. Create depth image ---
    {
        VkImageCreateInfo depthImageInfo{};
        depthImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depthImageInfo.imageType     = VK_IMAGE_TYPE_2D;
        depthImageInfo.extent        = {width, height, 1};
        depthImageInfo.mipLevels     = 1;
        depthImageInfo.arrayLayers   = 1;
        depthImageInfo.format        = VK_FORMAT_D32_SFLOAT;
        depthImageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthImageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depthImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        depthImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &depthImageInfo, nullptr, &m_offscreenDepthImage) != VK_SUCCESS)
            throw std::runtime_error("failed to create offscreen depth image!");

        VkMemoryRequirements depthMemReqs;
        vkGetImageMemoryRequirements(device, m_offscreenDepthImage, &depthMemReqs);

        VkMemoryAllocateInfo depthAllocInfo{};
        depthAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        depthAllocInfo.allocationSize  = depthMemReqs.size;
        depthAllocInfo.memoryTypeIndex = m_context.findMemoryType(depthMemReqs.memoryTypeBits,
                                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &depthAllocInfo, nullptr, &m_offscreenDepthMemory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate offscreen depth image memory!");

        vkBindImageMemory(device, m_offscreenDepthImage, m_offscreenDepthMemory, 0);

        // Create depth image view
        VkImageViewCreateInfo depthViewInfo{};
        depthViewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depthViewInfo.image                           = m_offscreenDepthImage;
        depthViewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format                          = VK_FORMAT_D32_SFLOAT;
        depthViewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthViewInfo.subresourceRange.baseMipLevel   = 0;
        depthViewInfo.subresourceRange.levelCount     = 1;
        depthViewInfo.subresourceRange.baseArrayLayer = 0;
        depthViewInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device, &depthViewInfo, nullptr, &m_offscreenDepthImageView) != VK_SUCCESS)
            throw std::runtime_error("failed to create offscreen depth image view!");
    }

    // --- 3. Create sampler (once, shared across resizes) ---
    if (m_offscreenSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter    = VK_FILTER_LINEAR;
        samplerInfo.minFilter    = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable        = VK_FALSE;
        samplerInfo.maxAnisotropy           = 1.0f;
        samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable           = VK_FALSE;
        samplerInfo.compareOp               = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias              = 0.0f;
        samplerInfo.minLod                  = 0.0f;
        samplerInfo.maxLod                  = 0.0f;

        if (vkCreateSampler(device, &samplerInfo, nullptr, &m_offscreenSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create offscreen sampler!");
    }

    // --- 4. Create offscreen render pass (once, shared across resizes) ---
    if (m_offscreenRenderPass == VK_NULL_HANDLE)
    {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format         = format;
        colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format         = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &colorRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        // Dependencies to ensure proper synchronization
        VkSubpassDependency dependencies[2] = {};

        dependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass      = 0;
        dependencies[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        dependencies[1].srcSubpass      = 0;
        dependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        rpInfo.pAttachments    = attachments.data();
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = dependencies;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &m_offscreenRenderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create offscreen render pass!");

        // Create pipeline compatible with offscreen render pass
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(PushConstantData);
        std::vector<VkPushConstantRange> pcRanges = { pcRange };

        std::vector<VkDescriptorSetLayout> setLayouts = {
            m_descriptor.getUboLayout(),
            m_descriptor.getTextureLayout()
        };
        m_offscreenPipeline.create(device, m_offscreenRenderPass,
                                   setLayouts, {width, height},
                                   pcRanges);

        m_wireframePipeline.create(device, m_offscreenRenderPass,
                                   setLayouts, {width, height},
                                   pcRanges, VK_POLYGON_MODE_LINE);

        // --- Create grid pipeline (no vertex input, alpha blending, UBO-only) ---
        {
            auto readFile = [](const std::string& filename) -> std::vector<char> {
                std::ifstream file(filename, std::ios::ate | std::ios::binary);
                if (!file.is_open())
                    throw std::runtime_error("failed to open file: " + filename);
                size_t fileSize = static_cast<size_t>(file.tellg());
                std::vector<char> buffer(fileSize);
                file.seekg(0);
                file.read(buffer.data(), fileSize);
                return buffer;
            };

            auto gridVertCode = readFile(std::string(ASSETS_DIR) + "/shaders/grid_vert.spv");
            auto gridFragCode = readFile(std::string(ASSETS_DIR) + "/shaders/grid_frag.spv");

            auto createShaderModule = [&](const std::vector<char>& code) -> VkShaderModule {
                VkShaderModuleCreateInfo ci{};
                ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                ci.codeSize = code.size();
                ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
                VkShaderModule mod;
                if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS)
                    throw std::runtime_error("failed to create grid shader module!");
                return mod;
            };

            VkShaderModule gridVert = createShaderModule(gridVertCode);
            VkShaderModule gridFrag = createShaderModule(gridFragCode);

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = gridVert;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = gridFrag;
            stages[1].pName = "main";

            // No vertex input
            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInput.vertexBindingDescriptionCount = 0;
            vertexInput.vertexAttributeDescriptionCount = 0;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            inputAssembly.primitiveRestartEnable = VK_FALSE;

            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.depthClampEnable = VK_FALSE;
            rasterizer.rasterizerDiscardEnable = VK_FALSE;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.lineWidth = 1.0f;
            rasterizer.cullMode = VK_CULL_MODE_NONE;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizer.depthBiasEnable = VK_FALSE;

            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            multisampling.sampleShadingEnable = VK_FALSE;

            // Alpha blending
            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            colorBlendAttachment.blendEnable = VK_TRUE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

            VkPipelineColorBlendStateCreateInfo colorBlending{};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.logicOpEnable = VK_FALSE;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &colorBlendAttachment;

            // Depth test + write enabled
            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
            depthStencil.depthBoundsTestEnable = VK_FALSE;
            depthStencil.stencilTestEnable = VK_FALSE;

            std::vector<VkDynamicState> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR
            };
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamicState.pDynamicStates = dynamicStates.data();

            // Pipeline layout: only UBO set (set 0), no push constants
            VkDescriptorSetLayout gridSetLayout = m_descriptor.getUboLayout();
            VkPipelineLayoutCreateInfo gridLayoutInfo{};
            gridLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            gridLayoutInfo.setLayoutCount = 1;
            gridLayoutInfo.pSetLayouts = &gridSetLayout;
            gridLayoutInfo.pushConstantRangeCount = 0;
            gridLayoutInfo.pPushConstantRanges = nullptr;

            if (vkCreatePipelineLayout(device, &gridLayoutInfo, nullptr, &m_gridPipelineLayout) != VK_SUCCESS)
                throw std::runtime_error("failed to create grid pipeline layout!");

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = 2;
            pipelineInfo.pStages = stages;
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = m_gridPipelineLayout;
            pipelineInfo.renderPass = m_offscreenRenderPass;
            pipelineInfo.subpass = 0;
            pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
            pipelineInfo.basePipelineIndex = -1;

            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_gridPipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create grid pipeline!");

            vkDestroyShaderModule(device, gridFrag, nullptr);
            vkDestroyShaderModule(device, gridVert, nullptr);
        }

        // Pass render pass and layout info to AssetManager for shader pipeline creation
        m_assetManager.setOffscreenRenderPass(m_offscreenRenderPass);
        m_assetManager.setDescriptorSetLayouts(m_descriptor.getUboLayout(), m_descriptor.getTextureLayout());
        m_assetManager.setOffscreenExtent({width, height});
    }

    // --- 5. Create framebuffer (color + depth) ---
    std::array<VkImageView, 2> fbAttachments = { m_offscreenImageView, m_offscreenDepthImageView };

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_offscreenRenderPass;
    fbInfo.attachmentCount = static_cast<uint32_t>(fbAttachments.size());
    fbInfo.pAttachments    = fbAttachments.data();
    fbInfo.width           = width;
    fbInfo.height          = height;
    fbInfo.layers          = 1;

    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_offscreenFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("failed to create offscreen framebuffer!");
}

void Renderer::resizeOffscreen(uint32_t width, uint32_t height)
{
    if (width == m_offscreenWidth && height == m_offscreenHeight && m_offscreenFramebuffer != VK_NULL_HANDLE)
        return;

    VkDevice device = m_context.getDevice();
    vkDeviceWaitIdle(device);

    // Destroy per-size resources (keep render pass, sampler, pipeline)
    if (m_offscreenFramebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(device, m_offscreenFramebuffer, nullptr);
        m_offscreenFramebuffer = VK_NULL_HANDLE;
    }

    if (m_offscreenDepthImageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, m_offscreenDepthImageView, nullptr);
        m_offscreenDepthImageView = VK_NULL_HANDLE;
    }

    if (m_offscreenDepthImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, m_offscreenDepthImage, nullptr);
        m_offscreenDepthImage = VK_NULL_HANDLE;
    }

    if (m_offscreenDepthMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, m_offscreenDepthMemory, nullptr);
        m_offscreenDepthMemory = VK_NULL_HANDLE;
    }

    if (m_offscreenImageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, m_offscreenImageView, nullptr);
        m_offscreenImageView = VK_NULL_HANDLE;
    }

    if (m_offscreenImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, m_offscreenImage, nullptr);
        m_offscreenImage = VK_NULL_HANDLE;
    }

    if (m_offscreenMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, m_offscreenMemory, nullptr);
        m_offscreenMemory = VK_NULL_HANDLE;
    }

    // Recreate with new size
    createOffscreen(width, height);
}

void Renderer::destroyOffscreen()
{
    VkDevice device = m_context.getDevice();

    if (m_offscreenFramebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(device, m_offscreenFramebuffer, nullptr);
        m_offscreenFramebuffer = VK_NULL_HANDLE;
    }

    if (m_offscreenDepthImageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, m_offscreenDepthImageView, nullptr);
        m_offscreenDepthImageView = VK_NULL_HANDLE;
    }

    if (m_offscreenDepthImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, m_offscreenDepthImage, nullptr);
        m_offscreenDepthImage = VK_NULL_HANDLE;
    }

    if (m_offscreenDepthMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, m_offscreenDepthMemory, nullptr);
        m_offscreenDepthMemory = VK_NULL_HANDLE;
    }

    if (m_offscreenImageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, m_offscreenImageView, nullptr);
        m_offscreenImageView = VK_NULL_HANDLE;
    }

    if (m_offscreenImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, m_offscreenImage, nullptr);
        m_offscreenImage = VK_NULL_HANDLE;
    }

    if (m_offscreenMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, m_offscreenMemory, nullptr);
        m_offscreenMemory = VK_NULL_HANDLE;
    }

    if (m_offscreenSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(device, m_offscreenSampler, nullptr);
        m_offscreenSampler = VK_NULL_HANDLE;
    }

    if (m_offscreenRenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(device, m_offscreenRenderPass, nullptr);
        m_offscreenRenderPass = VK_NULL_HANDLE;
    }

    m_offscreenWidth  = 0;
    m_offscreenHeight = 0;
}

void Renderer::drawSceneToOffscreen(VkCommandBuffer commandBuffer, Scene& scene)
{
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass        = m_offscreenRenderPass;
    renderPassInfo.framebuffer       = m_offscreenFramebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_offscreenWidth, m_offscreenHeight};

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.4f, 0.4f, 0.45f, 1.0f}};  // match sky bottom color
    clearValues[1].depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues    = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(m_offscreenWidth);
    viewport.height   = static_cast<float>(m_offscreenHeight);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_offscreenWidth, m_offscreenHeight};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // --- Draw grid + sky (before scene objects) ---
    if (m_gridPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
        VkDescriptorSet gridUboSet = m_descriptor.getUboSet(m_currentFrame);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_gridPipelineLayout, 0, 1, &gridUboSet, 0, nullptr);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    // UBO descriptor set (set 0) - shared across all nodes
    VkDescriptorSet uboSet = m_descriptor.getUboSet(m_currentFrame);

    // Render each scene node with its own material, pipeline, and mesh
    scene.traverseNodes([&](Node* node) {
        // Skip light nodes during mesh rendering
        if (node->nodeType == NodeType::DirectionalLight) return;

        // Load material (or use defaults)
        const MaterialAsset* mat = nullptr;
        if (!node->materialPath.empty())
            mat = m_assetManager.loadMaterial(node->materialPath);

        // Bind shader pipeline — different nodes may use different shaders
        VkPipeline shaderPipeline = (mat && mat->shader)
            ? mat->shader->pipeline.getPipeline()
            : m_offscreenPipeline.getPipeline();
        VkPipelineLayout pipelineLayout = (mat && mat->shader)
            ? mat->shader->pipeline.getPipelineLayout()
            : m_offscreenPipeline.getPipelineLayout();
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shaderPipeline);

        // Re-bind UBO descriptor set (set 0) after pipeline change
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 0, 1, &uboSet, 0, nullptr);

        // Bind material texture set (set 1) or default
        VkDescriptorSet texSet = (mat && mat->textureSet != VK_NULL_HANDLE)
            ? mat->textureSet
            : m_defaultMaterialTexSet;
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 1, 1, &texSet, 0, nullptr);

        // Push constants from material (or defaults)
        PushConstantData pc{};
        pc.model = node->getWorldMatrix();
        pc.baseColor = mat ? mat->baseColor : glm::vec4(1.0f);
        pc.metallic = mat ? mat->metallic : 0.0f;
        pc.roughness = mat ? mat->roughness : 0.5f;
        pc.highlighted = 0;  // wireframe pass handles highlight
        vkCmdPushConstants(commandBuffer, pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(PushConstantData), &pc);

        // Determine mesh and draw
        if (!node->meshPath.empty()) {
            auto* mesh = m_assetManager.loadMesh(node->meshPath);
            if (mesh) {
                VkBuffer buffers[] = {mesh->vertexBuffer};
                VkDeviceSize offsets[] = {0};
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);
                vkCmdBindIndexBuffer(commandBuffer, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(commandBuffer, mesh->indexCount, 1, 0, 0, 0);
            }
        } else if (node->meshType != MeshType::None) {
            m_meshLibrary.bind(commandBuffer, node->meshType);
            vkCmdDrawIndexed(commandBuffer, m_meshLibrary.getIndexCount(node->meshType), 1, 0, 0, 0);
        }
    });

    // Draw wireframe outline for selected node
    Node* selected = scene.getSelectedNode();
    if (selected && (selected->meshType != MeshType::None || !selected->meshPath.empty())) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline.getPipeline());

        // Re-bind UBO set 0 after pipeline change, bind texture set 1
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_wireframePipeline.getPipelineLayout(), 0, 1,
                                &uboSet, 0, nullptr);
        VkDescriptorSet texSet = m_defaultTextureSet;
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_wireframePipeline.getPipelineLayout(), 1, 1,
                                &texSet, 0, nullptr);

        PushConstantData pc{};
        pc.model = selected->getWorldMatrix();
        pc.baseColor = glm::vec4(1.0f, 0.5f, 0.0f, 1.0f); // orange outline
        pc.metallic = 0.0f;
        pc.roughness = 1.0f;
        pc.highlighted = 1;
        vkCmdPushConstants(commandBuffer, m_wireframePipeline.getPipelineLayout(),
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(PushConstantData), &pc);

        if (!selected->meshPath.empty()) {
            auto* mesh = m_assetManager.loadMesh(selected->meshPath);
            if (mesh) {
                VkBuffer buffers[] = {mesh->vertexBuffer};
                VkDeviceSize offsets[] = {0};
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);
                vkCmdBindIndexBuffer(commandBuffer, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(commandBuffer, mesh->indexCount, 1, 0, 0, 0);
            }
        } else {
            m_meshLibrary.bind(commandBuffer, selected->meshType);
            vkCmdDrawIndexed(commandBuffer, m_meshLibrary.getIndexCount(selected->meshType), 1, 0, 0, 0);
        }
    }

    // Draw light gizmos (small wireframe sphere for each directional light)
    scene.traverseNodes([&](Node* node) {
        if (node->nodeType != NodeType::DirectionalLight) return;

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_wireframePipeline.getPipeline());

        VkDescriptorSet uboSet2 = m_descriptor.getUboSet(m_currentFrame);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_wireframePipeline.getPipelineLayout(), 0, 1,
                                &uboSet2, 0, nullptr);

        VkDescriptorSet texSet = m_defaultMaterialTexSet;
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_wireframePipeline.getPipelineLayout(), 1, 1,
                                &texSet, 0, nullptr);

        // Small sphere at light position, scaled down
        glm::mat4 model = glm::translate(glm::mat4(1.0f), node->transform.position);
        model = glm::scale(model, glm::vec3(0.2f));

        PushConstantData pc{};
        pc.model = model;
        pc.baseColor = glm::vec4(node->lightColor, 1.0f);
        pc.highlighted = 1;
        vkCmdPushConstants(commandBuffer, m_wireframePipeline.getPipelineLayout(),
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(PushConstantData), &pc);

        m_meshLibrary.bind(commandBuffer, MeshType::Sphere);
        vkCmdDrawIndexed(commandBuffer, m_meshLibrary.getIndexCount(MeshType::Sphere), 1, 0, 0, 0);

        // Draw direction arrow using a scaled cube along light direction
        glm::vec3 dir = node->getLightDirection();
        glm::vec3 arrowEnd = node->transform.position + dir * 0.5f;
        glm::vec3 arrowCenter = (node->transform.position + arrowEnd) * 0.5f;

        glm::mat4 arrowModel = glm::translate(glm::mat4(1.0f), arrowCenter);
        // Scale thin along perpendicular axes, long along direction
        arrowModel = glm::scale(arrowModel, glm::vec3(0.02f, 0.02f, 0.25f));
        // Rotate cube to align with light direction
        glm::vec3 up = glm::vec3(0, 0, -1);
        if (glm::abs(glm::dot(dir, up)) > 0.99f)
            up = glm::vec3(1, 0, 0);
        glm::mat4 lookMat = glm::inverse(glm::lookAt(glm::vec3(0), dir, up));
        arrowModel = glm::translate(glm::mat4(1.0f), arrowCenter) * lookMat * glm::scale(glm::mat4(1.0f), glm::vec3(0.02f, 0.02f, 0.25f));

        pc.model = arrowModel;
        vkCmdPushConstants(commandBuffer, m_wireframePipeline.getPipelineLayout(),
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(PushConstantData), &pc);

        m_meshLibrary.bind(commandBuffer, MeshType::Cube);
        vkCmdDrawIndexed(commandBuffer, m_meshLibrary.getIndexCount(MeshType::Cube), 1, 0, 0, 0);
    });

    vkCmdEndRenderPass(commandBuffer);
}

// ---------------------------------------------------------------------------
// Old recordCommandBuffer -- no longer used for swapchain scene rendering.
// ---------------------------------------------------------------------------
void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex)
{
    // No longer used. Scene rendering goes through drawSceneToOffscreen().
}

void Renderer::updateUniformBuffer(uint32_t currentImage)
{
    UniformBufferObject ubo{};
    float aspect = (m_offscreenWidth > 0)
        ? m_offscreenWidth / static_cast<float>(m_offscreenHeight)
        : 1.0f;

    if (m_camera) {
        ubo.view = m_camera->getViewMatrix();
        ubo.proj = m_camera->getProjMatrix(aspect);
    } else {
        ubo.view = glm::lookAt(glm::vec3(2,2,2), glm::vec3(0,0,0), glm::vec3(0,1,0));
        ubo.proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
        ubo.proj[1][1] *= -1;
    }

    // Find directional light in scene (use first one found, fallback to defaults)
    ubo.lightDir = glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f));
    ubo.lightColor = glm::vec3(1.0f, 1.0f, 1.0f);
    ubo.ambientColor = glm::vec3(0.15f, 0.15f, 0.15f);
    if (m_activeScene) {
        m_activeScene->traverseNodes([&](Node* node) {
            if (node->nodeType == NodeType::DirectionalLight) {
                ubo.lightDir = node->getLightDirection();
                ubo.lightColor = node->lightColor * node->lightIntensity;
            }
        });
    }
    if (m_camera)
        ubo.cameraPos = m_camera->getPosition();
    else
        ubo.cameraPos = glm::vec3(2.0f, 2.0f, 2.0f);

    memcpy(m_buffer.getUniformBuffersMapped()[currentImage], &ubo, sizeof(ubo));
}

void Renderer::recreateSwapChain()
{
    // Wait until window is non-zero size
    GLFWwindow* nativeWindow = m_window->getNativeWindow();
    int width = 0, height = 0;
    glfwGetFramebufferSize(nativeWindow, &width, &height);
    while (width == 0 || height == 0)
    {
        glfwGetFramebufferSize(nativeWindow, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(m_context.getDevice());

    m_swapChain.cleanup(m_context.getDevice());

    m_swapChain.create(m_context, nativeWindow);
    m_swapChain.createFramebuffers(m_context.getDevice(), m_renderPass.get());

    if (m_swapChainRecreatedCb)
        m_swapChainRecreatedCb();
}

} // namespace QymEngine
