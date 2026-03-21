#include "renderer/Renderer.h"
#include "core/Window.h"
#include "scene/Frustum.h"

#include <SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <iostream>
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

    SDL_Window* nativeWindow = m_window->getNativeWindow();

    // Init order follows dependency chain
    m_context.init(nativeWindow);
    m_swapChain.create(m_context, nativeWindow);
    m_commandManager.createPool(m_context);

    // The original scene RenderPass (used for swapchain). Kept for init compatibility.
    m_renderPass.create(m_context.getDevice(), m_swapChain.getImageFormat());

    // Register per-frame layout in cache (set 0: UBO with vertex+fragment stages)
    {
        VkDescriptorSetLayoutBinding uboBinding{};
        uboBinding.binding = 0;
        uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboBinding.descriptorCount = 1;
        uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        uboBinding.pImmutableSamplers = nullptr;
        m_perFrameLayout = m_layoutCache.getOrCreate(m_context.getDevice(), {uboBinding});
    }

    // Create main pipeline using layout cache
    m_pipeline.create(m_context.getDevice(), m_renderPass.get(),
                      m_swapChain.getExtent(), m_layoutCache);

    // Swapchain framebuffers
    m_swapChain.createFramebuffers(m_context.getDevice(), m_renderPass.get());

    m_texture.createTextureImage(m_context, m_commandManager);
    m_texture.createTextureImageView(m_context.getDevice());
    m_texture.createTextureSampler(m_context);

    m_meshLibrary.init(m_context, m_commandManager);
    m_buffer.createUniformBuffers(m_context, MAX_FRAMES_IN_FLIGHT);

    // Create descriptor pool
    m_descriptor.createPool(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT, 100);

    // Create per-frame UBO descriptor sets (set 0)
    m_descriptor.createPerFrameSets(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT,
                                     m_perFrameLayout,
                                     m_buffer.getUniformBuffers());

    // Create fallback textures for the material system
    createFallbackTextures();

    // Create bindless descriptor resources (PC only, after fallback textures)
    createBindlessResources();

    // Initialize AssetManager with layout cache and descriptor allocator
    m_assetManager.init(m_context, m_commandManager);
    m_assetManager.setLayoutCache(&m_layoutCache);
    m_assetManager.setDescriptorAllocator(&m_descriptor);
    m_assetManager.setFallbackAlbedo(m_whiteFallbackView, m_fallbackSampler);
    m_assetManager.setFallbackNormal(m_normalFallbackView, m_fallbackSampler);
    m_assetManager.setRenderer(this);

    // For texture preview in Inspector (single-sampler layout)
    {
        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding = 0;
        samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayout texPreviewLayout = m_layoutCache.getOrCreate(
            m_context.getDevice(), {samplerBinding});
        m_assetManager.setTextureDescriptorSetLayout(texPreviewLayout);
        m_assetManager.setTextureDescriptorPool(m_descriptor.getPool());
    }

#ifndef __ANDROID__
    m_assetManager.scanAssets(std::string(ASSETS_DIR));
#endif

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

void Renderer::blitToSwapchain()
{
    if (!isOffscreenReady()) return;

    VkCommandBuffer cmd = m_commandManager.getBuffer(m_currentFrame);
    VkImage swapImage = m_swapChain.getImages()[m_currentImageIndex];

    // Transition offscreen image: SHADER_READ_ONLY -> TRANSFER_SRC
    VkImageMemoryBarrier offscreenBarrier{};
    offscreenBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    offscreenBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    offscreenBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    offscreenBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    offscreenBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    offscreenBarrier.image = m_offscreenImage;
    offscreenBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    offscreenBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    offscreenBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    // Transition swapchain image: UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier swapBarrier{};
    swapBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    swapBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    swapBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    swapBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.image = swapImage;
    swapBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    swapBarrier.srcAccessMask = 0;
    swapBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier barriers[] = {offscreenBarrier, swapBarrier};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);

    // Blit offscreen -> swapchain
    VkImageBlit blitRegion{};
    blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.srcOffsets[0] = {0, 0, 0};
    blitRegion.srcOffsets[1] = {(int32_t)m_offscreenWidth, (int32_t)m_offscreenHeight, 1};
    blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.dstOffsets[0] = {0, 0, 0};
    VkExtent2D swapExtent = m_swapChain.getExtent();
    blitRegion.dstOffsets[1] = {(int32_t)swapExtent.width, (int32_t)swapExtent.height, 1};

    vkCmdBlitImage(cmd,
        m_offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitRegion, VK_FILTER_LINEAR);

    // Transition offscreen back: TRANSFER_SRC -> SHADER_READ_ONLY
    offscreenBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    offscreenBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    offscreenBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    offscreenBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    // Transition swapchain: TRANSFER_DST -> PRESENT_SRC
    swapBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    swapBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    swapBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    swapBarrier.dstAccessMask = 0;

    VkImageMemoryBarrier barriers2[] = {offscreenBarrier, swapBarrier};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers2);
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
}

// ---------------------------------------------------------------------------
// Bindless descriptor resources (PC only)
// ---------------------------------------------------------------------------

void Renderer::createBindlessResources()
{
#ifndef __ANDROID__
    if (!m_context.supportsBindless()) {
        m_bindlessEnabled = false;
        std::cout << "Bindless: disabled (GPU does not support required features)" << std::endl;
        return;
    }

    m_bindlessEnabled = true;
    VkDevice device = m_context.getDevice();

    // --- 1. Create separate descriptor pool with UPDATE_AFTER_BIND ---
    {
        std::array<VkDescriptorPoolSize, 3> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        poolSizes[0].descriptorCount = MAX_BINDLESS_SAMPLERS;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        poolSizes[1].descriptorCount = MAX_BINDLESS_TEXTURES;
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[2].descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = 1;

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_bindlessPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create bindless descriptor pool!");
    }

    // --- 2. Create bindless set layout ---
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        // binding 0: sampler array
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[0].descriptorCount = MAX_BINDLESS_SAMPLERS;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[0].pImmutableSamplers = nullptr;

        // binding 1: texture array (variable count)
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[1].descriptorCount = MAX_BINDLESS_TEXTURES;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].pImmutableSamplers = nullptr;

        // binding 2: material SSBO
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].pImmutableSamplers = nullptr;

        // Binding flags for bindless
        std::array<VkDescriptorBindingFlags, 3> bindingFlags{};
        bindingFlags[0] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        bindingFlags[1] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                          VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        bindingFlags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
        bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bindingFlagsInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        bindingFlagsInfo.pBindingFlags = bindingFlags.data();

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.pNext = &bindingFlagsInfo;

        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_bindlessSetLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create bindless descriptor set layout!");
    }

    // --- 3. Allocate the bindless descriptor set with variable count ---
    {
        uint32_t variableCount = MAX_BINDLESS_TEXTURES;
        VkDescriptorSetVariableDescriptorCountAllocateInfo varCountInfo{};
        varCountInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        varCountInfo.descriptorSetCount = 1;
        varCountInfo.pDescriptorCounts = &variableCount;

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_bindlessPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_bindlessSetLayout;
        allocInfo.pNext = &varCountInfo;

        if (vkAllocateDescriptorSets(device, &allocInfo, &m_bindlessSet) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate bindless descriptor set!");
    }

    // --- 4. Create material SSBO (host visible, persistently mapped) ---
    {
        VkDeviceSize ssboSize = sizeof(BindlessMaterialEntry) * MAX_MATERIALS;
        Buffer::createBuffer(m_context, ssboSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m_materialSSBO, m_materialSSBOMemory);

        vkMapMemory(device, m_materialSSBOMemory, 0, ssboSize, 0, &m_materialSSBOMapped);
        memset(m_materialSSBOMapped, 0, ssboSize);

        // Write SSBO to descriptor set (binding 2)
        VkDescriptorBufferInfo ssboInfo{};
        ssboInfo.buffer = m_materialSSBO;
        ssboInfo.offset = 0;
        ssboInfo.range = ssboSize;

        VkWriteDescriptorSet ssboWrite{};
        ssboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ssboWrite.dstSet = m_bindlessSet;
        ssboWrite.dstBinding = 2;
        ssboWrite.dstArrayElement = 0;
        ssboWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ssboWrite.descriptorCount = 1;
        ssboWrite.pBufferInfo = &ssboInfo;

        vkUpdateDescriptorSets(device, 1, &ssboWrite, 0, nullptr);
    }

    // --- 5. Register default sampler and fallback textures ---
    m_nextSamplerIndex = 0;
    m_nextTextureIndex = 0;
    m_nextMaterialIndex = 0;

    // Register fallback sampler at index 0
    registerBindlessSampler(m_fallbackSampler);

    // Register fallback textures (white=0, normal=1)
    uint32_t whiteFallbackIdx = registerBindlessTexture(m_whiteFallbackView);
    uint32_t normalFallbackIdx = registerBindlessTexture(m_normalFallbackView);

    // --- 6. Create default bindless material entry (index 0) ---
    m_defaultBindlessMaterialIndex = allocateBindlessMaterialIndex();
    {
        BindlessMaterialEntry defaultEntry{};
        defaultEntry.baseColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        defaultEntry.metallic = 0.0f;
        defaultEntry.roughness = 0.5f;
        defaultEntry.albedoTexIndex = whiteFallbackIdx;
        defaultEntry.normalTexIndex = normalFallbackIdx;
        defaultEntry.samplerIndex = 0;
        updateBindlessMaterialEntry(m_defaultBindlessMaterialIndex, defaultEntry);
    }

    // --- 7. Create wireframe bindless material entry ---
    m_wireframeBindlessMaterialIndex = allocateBindlessMaterialIndex();
    {
        BindlessMaterialEntry wireframeEntry{};
        wireframeEntry.baseColor = glm::vec4(1.0f, 0.5f, 0.0f, 1.0f);
        wireframeEntry.metallic = 0.0f;
        wireframeEntry.roughness = 1.0f;
        wireframeEntry.albedoTexIndex = whiteFallbackIdx;
        wireframeEntry.normalTexIndex = normalFallbackIdx;
        wireframeEntry.samplerIndex = 0;
        updateBindlessMaterialEntry(m_wireframeBindlessMaterialIndex, wireframeEntry);
    }

    std::cout << "Bindless: enabled (samplers=" << m_nextSamplerIndex
              << ", textures=" << m_nextTextureIndex
              << ", materials=" << m_nextMaterialIndex << ")" << std::endl;
#else
    m_bindlessEnabled = false;
#endif
}

void Renderer::destroyBindlessResources()
{
    VkDevice device = m_context.getDevice();

    m_bindlessOffscreenPipeline.cleanup(device);
    m_bindlessWireframePipeline.cleanup(device);

    if (m_materialSSBO != VK_NULL_HANDLE) {
        if (m_materialSSBOMapped) {
            vkUnmapMemory(device, m_materialSSBOMemory);
            m_materialSSBOMapped = nullptr;
        }
        vkDestroyBuffer(device, m_materialSSBO, nullptr);
        m_materialSSBO = VK_NULL_HANDLE;
    }
    if (m_materialSSBOMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_materialSSBOMemory, nullptr);
        m_materialSSBOMemory = VK_NULL_HANDLE;
    }

    if (m_bindlessSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_bindlessSetLayout, nullptr);
        m_bindlessSetLayout = VK_NULL_HANDLE;
    }
    if (m_bindlessPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_bindlessPool, nullptr);
        m_bindlessPool = VK_NULL_HANDLE;
    }

    m_bindlessSet = VK_NULL_HANDLE;
    m_bindlessEnabled = false;
    m_nextTextureIndex = 0;
    m_nextSamplerIndex = 0;
    m_nextMaterialIndex = 0;
}

uint32_t Renderer::registerBindlessTexture(VkImageView view)
{
    if (!m_bindlessEnabled || m_nextTextureIndex >= MAX_BINDLESS_TEXTURES) {
        std::cerr << "Bindless: cannot register texture (disabled or limit reached)" << std::endl;
        return 0;
    }

    uint32_t index = m_nextTextureIndex++;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_bindlessSet;
    write.dstBinding = 1;
    write.dstArrayElement = index;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_context.getDevice(), 1, &write, 0, nullptr);
    return index;
}

uint32_t Renderer::registerBindlessSampler(VkSampler sampler)
{
    if (!m_bindlessEnabled || m_nextSamplerIndex >= MAX_BINDLESS_SAMPLERS) {
        std::cerr << "Bindless: cannot register sampler (disabled or limit reached)" << std::endl;
        return 0;
    }

    uint32_t index = m_nextSamplerIndex++;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_bindlessSet;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_context.getDevice(), 1, &write, 0, nullptr);
    return index;
}

void Renderer::updateBindlessMaterialEntry(uint32_t index, const BindlessMaterialEntry& entry)
{
    if (!m_bindlessEnabled || !m_materialSSBOMapped || index >= MAX_MATERIALS) return;

    auto* entries = static_cast<BindlessMaterialEntry*>(m_materialSSBOMapped);
    entries[index] = entry;
}

uint32_t Renderer::allocateBindlessMaterialIndex()
{
    if (!m_bindlessEnabled || m_nextMaterialIndex >= MAX_MATERIALS) {
        std::cerr << "Bindless: cannot allocate material index (disabled or limit reached)" << std::endl;
        return 0;
    }
    return m_nextMaterialIndex++;
}

void Renderer::shutdown()
{
    vkDeviceWaitIdle(m_context.getDevice());

    VkDevice device = m_context.getDevice();

    destroyOffscreen();

    // Cleanup bindless resources (before fallback textures they reference)
    destroyBindlessResources();

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

    // Destroy default material param buffer
    if (m_defaultMaterialParamBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, m_defaultMaterialParamBuffer, nullptr);
    if (m_defaultMaterialParamMemory != VK_NULL_HANDLE) {
        if (m_defaultMaterialParamMapped)
            vkUnmapMemory(device, m_defaultMaterialParamMemory);
        vkFreeMemory(device, m_defaultMaterialParamMemory, nullptr);
    }

    // Destroy wireframe material param buffer
    if (m_wireframeMaterialParamBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, m_wireframeMaterialParamBuffer, nullptr);
    if (m_wireframeMaterialParamMemory != VK_NULL_HANDLE) {
        if (m_wireframeMaterialParamMapped)
            vkUnmapMemory(device, m_wireframeMaterialParamMemory);
        vkFreeMemory(device, m_wireframeMaterialParamMemory, nullptr);
    }

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
    m_wireframePipeline.cleanup(device);
    m_pipeline.cleanup(device);
    m_renderPass.cleanup(device);
    // Layout cache cleanup AFTER all pipelines and descriptors
    m_layoutCache.cleanup(device);
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
    imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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

        // Create offscreen pipeline using layout cache
        m_offscreenPipeline.create(device, m_offscreenRenderPass,
            {width, height}, m_layoutCache);

        // Create wireframe pipeline using layout cache (same shader, different polygon mode)
        m_wireframePipeline.create(device, m_offscreenRenderPass,
            {width, height}, m_layoutCache, VK_POLYGON_MODE_LINE);

        // Create bindless pipelines (if enabled)
        if (m_bindlessEnabled && m_bindlessSetLayout != VK_NULL_HANDLE) {
            // Build layouts: set 0 = per-frame (same), set 1 = bindless layout
            std::vector<VkDescriptorSetLayout> bindlessLayouts = {
                m_perFrameLayout,
                m_bindlessSetLayout
            };

            // Use the bindless reflection JSON for push constant ranges
            // Load it to get the correct push constant data
            ShaderReflectionData bindlessReflection;
            std::string bindlessReflectPath = std::string(ASSETS_DIR) + "/shaders/Triangle_bindless.reflect.json";
            if (bindlessReflection.loadFromJson(bindlessReflectPath)) {
                auto pcRanges = bindlessReflection.createPushConstantRanges();

                m_bindlessOffscreenPipeline.createWithLayouts(
                    device, m_offscreenRenderPass, bindlessLayouts,
                    {width, height}, pcRanges, VK_POLYGON_MODE_FILL,
                    "shaders/triangle_vert_bindless.spv",
                    "shaders/triangle_frag_bindless.spv");

                m_bindlessWireframePipeline.createWithLayouts(
                    device, m_offscreenRenderPass, bindlessLayouts,
                    {width, height}, pcRanges, VK_POLYGON_MODE_LINE,
                    "shaders/triangle_vert_bindless.spv",
                    "shaders/triangle_frag_bindless.spv");
            } else {
                std::cerr << "Bindless: failed to load reflection JSON, disabling bindless pipelines" << std::endl;
                m_bindlessEnabled = false;
            }
        }

        // --- Create grid pipeline (no vertex input, alpha blending, UBO-only) ---
        {
            auto readFile = [](const std::string& filename) -> std::vector<char> {
                SDL_RWops* rw = SDL_RWFromFile(filename.c_str(), "rb");
                if (!rw)
                    throw std::runtime_error("failed to open file: " + filename);
                Sint64 size = SDL_RWsize(rw);
                std::vector<char> buffer(static_cast<size_t>(size));
                SDL_RWread(rw, buffer.data(), 1, static_cast<size_t>(size));
                SDL_RWclose(rw);
                return buffer;
            };

#ifdef __ANDROID__
            auto gridVertCode = readFile("shaders/grid_vert.spv");
            auto gridFragCode = readFile("shaders/grid_frag.spv");
#else
            auto gridVertCode = readFile(std::string(ASSETS_DIR) + "/shaders/grid_vert.spv");
            auto gridFragCode = readFile(std::string(ASSETS_DIR) + "/shaders/grid_frag.spv");
#endif

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

            // Pipeline layout: only UBO set (set 0) from layout cache, no push constants
            VkDescriptorSetLayout gridSetLayout = m_perFrameLayout;
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

        // --- Create default material descriptor set (for nodes without material) ---
        {
            // Get set 1 layout from offscreen pipeline (Lit shader)
            const auto& pipelineLayouts = m_offscreenPipeline.getDescriptorSetLayouts();
            if (pipelineLayouts.size() >= 2) {
                VkDescriptorSetLayout set1Layout = pipelineLayouts[1];
                m_defaultMaterialSet = m_descriptor.allocateSet(device, set1Layout);

                // Create default material params UBO (match Lit material params: baseColor + metallic + roughness = 32 bytes)
                uint32_t defaultParamSize = 32;
                Buffer::createBuffer(m_context, defaultParamSize,
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    m_defaultMaterialParamBuffer, m_defaultMaterialParamMemory);
                vkMapMemory(device, m_defaultMaterialParamMemory, 0, defaultParamSize, 0, &m_defaultMaterialParamMapped);

                // Default: white baseColor, 0 metallic, 0.5 roughness
                struct DefaultParams {
                    glm::vec4 baseColor;
                    float metallic;
                    float roughness;
                    float _pad[2];
                } defaults;
                defaults.baseColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                defaults.metallic = 0.0f;
                defaults.roughness = 0.5f;
                defaults._pad[0] = 0.0f;
                defaults._pad[1] = 0.0f;
                memcpy(m_defaultMaterialParamMapped, &defaults, sizeof(defaults));

                // Write UBO binding (binding 0)
                VkDescriptorBufferInfo bufferInfo{};
                bufferInfo.buffer = m_defaultMaterialParamBuffer;
                bufferInfo.offset = 0;
                bufferInfo.range = defaultParamSize;

                VkWriteDescriptorSet uboWrite{};
                uboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                uboWrite.dstSet = m_defaultMaterialSet;
                uboWrite.dstBinding = 0;
                uboWrite.dstArrayElement = 0;
                uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                uboWrite.descriptorCount = 1;
                uboWrite.pBufferInfo = &bufferInfo;

                vkUpdateDescriptorSets(device, 1, &uboWrite, 0, nullptr);

                // Write sampler bindings (albedo at binding 1, normal at binding 2) using fallback textures
                VkDescriptorImageInfo albedoImageInfo{};
                albedoImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                albedoImageInfo.imageView = m_whiteFallbackView;
                albedoImageInfo.sampler = m_fallbackSampler;

                VkDescriptorImageInfo normalImageInfo{};
                normalImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                normalImageInfo.imageView = m_normalFallbackView;
                normalImageInfo.sampler = m_fallbackSampler;

                std::array<VkWriteDescriptorSet, 2> samplerWrites{};
                samplerWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                samplerWrites[0].dstSet = m_defaultMaterialSet;
                samplerWrites[0].dstBinding = 1;
                samplerWrites[0].dstArrayElement = 0;
                samplerWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                samplerWrites[0].descriptorCount = 1;
                samplerWrites[0].pImageInfo = &albedoImageInfo;

                samplerWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                samplerWrites[1].dstSet = m_defaultMaterialSet;
                samplerWrites[1].dstBinding = 2;
                samplerWrites[1].dstArrayElement = 0;
                samplerWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                samplerWrites[1].descriptorCount = 1;
                samplerWrites[1].pImageInfo = &normalImageInfo;

                vkUpdateDescriptorSets(device, static_cast<uint32_t>(samplerWrites.size()),
                                       samplerWrites.data(), 0, nullptr);
            }
        }

        // Create wireframe material set (orange baseColor for selection highlight)
        const auto& wfLayouts = m_offscreenPipeline.getDescriptorSetLayouts();
        if (m_wireframeMaterialSet == VK_NULL_HANDLE && wfLayouts.size() >= 2) {
            VkDescriptorSetLayout set1Layout = wfLayouts[1];
            m_wireframeMaterialSet = m_descriptor.allocateSet(device, set1Layout);

            uint32_t wfParamSize = 32;
            Buffer::createBuffer(m_context, wfParamSize,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_wireframeMaterialParamBuffer, m_wireframeMaterialParamMemory);
            vkMapMemory(device, m_wireframeMaterialParamMemory, 0, wfParamSize, 0, &m_wireframeMaterialParamMapped);

            struct { glm::vec4 baseColor; float metallic; float roughness; float _pad[2]; } wfParams;
            wfParams.baseColor = glm::vec4(1.0f, 0.5f, 0.0f, 1.0f); // orange
            wfParams.metallic = 0.0f;
            wfParams.roughness = 1.0f;
            wfParams._pad[0] = 0.0f;
            wfParams._pad[1] = 0.0f;
            memcpy(m_wireframeMaterialParamMapped, &wfParams, sizeof(wfParams));

            VkDescriptorBufferInfo wfBufInfo{};
            wfBufInfo.buffer = m_wireframeMaterialParamBuffer;
            wfBufInfo.offset = 0;
            wfBufInfo.range = wfParamSize;

            VkWriteDescriptorSet wfUboWrite{};
            wfUboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wfUboWrite.dstSet = m_wireframeMaterialSet;
            wfUboWrite.dstBinding = 0;
            wfUboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wfUboWrite.descriptorCount = 1;
            wfUboWrite.pBufferInfo = &wfBufInfo;
            vkUpdateDescriptorSets(device, 1, &wfUboWrite, 0, nullptr);

            // Bind fallback textures for wireframe material
            VkDescriptorImageInfo wfAlbedo{};
            wfAlbedo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            wfAlbedo.imageView = m_whiteFallbackView;
            wfAlbedo.sampler = m_fallbackSampler;
            VkDescriptorImageInfo wfNormal{};
            wfNormal.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            wfNormal.imageView = m_normalFallbackView;
            wfNormal.sampler = m_fallbackSampler;

            std::array<VkWriteDescriptorSet, 2> wfSamplerWrites{};
            wfSamplerWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wfSamplerWrites[0].dstSet = m_wireframeMaterialSet;
            wfSamplerWrites[0].dstBinding = 1;
            wfSamplerWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wfSamplerWrites[0].descriptorCount = 1;
            wfSamplerWrites[0].pImageInfo = &wfAlbedo;
            wfSamplerWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wfSamplerWrites[1].dstSet = m_wireframeMaterialSet;
            wfSamplerWrites[1].dstBinding = 2;
            wfSamplerWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wfSamplerWrites[1].descriptorCount = 1;
            wfSamplerWrites[1].pImageInfo = &wfNormal;
            vkUpdateDescriptorSets(device, 2, wfSamplerWrites.data(), 0, nullptr);
        }

        // Pass render pass and layout info to AssetManager for shader pipeline creation
        m_assetManager.setOffscreenRenderPass(m_offscreenRenderPass);
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

void Renderer::reloadShaders()
{
    VkDevice device = m_context.getDevice();
    vkDeviceWaitIdle(device);

    // 1. Run shader compiler
    std::string compilerPath;
#ifdef _WIN32
    compilerPath = std::string(ASSETS_DIR) + "/../build3/tools/shader_compiler/Debug/ShaderCompiler.exe";
#else
    compilerPath = "ShaderCompiler";
#endif
    std::string shadersDir = std::string(ASSETS_DIR) + "/shaders";
    std::string cmd = "\"" + compilerPath + "\" \"" + shadersDir + "\" \"" + shadersDir + "\"";
    int result = system(cmd.c_str());
    (void)result;

    // 2. Invalidate all shader and material caches
    m_assetManager.invalidateAllShadersAndMaterials();

    // 3. Rebuild offscreen and wireframe pipelines
    m_offscreenPipeline.cleanup(device);
    m_wireframePipeline.cleanup(device);
    m_offscreenPipeline.create(device, m_offscreenRenderPass,
        {m_offscreenWidth, m_offscreenHeight}, m_layoutCache);
    m_wireframePipeline.create(device, m_offscreenRenderPass,
        {m_offscreenWidth, m_offscreenHeight}, m_layoutCache, VK_POLYGON_MODE_LINE);

    // 3b. Rebuild bindless pipelines
    if (m_bindlessEnabled && m_bindlessSetLayout != VK_NULL_HANDLE) {
        m_bindlessOffscreenPipeline.cleanup(device);
        m_bindlessWireframePipeline.cleanup(device);

        std::vector<VkDescriptorSetLayout> bindlessLayouts = {
            m_perFrameLayout,
            m_bindlessSetLayout
        };

        ShaderReflectionData bindlessReflection;
        std::string bindlessReflectPath = std::string(ASSETS_DIR) + "/shaders/Triangle_bindless.reflect.json";
        if (bindlessReflection.loadFromJson(bindlessReflectPath)) {
            auto pcRanges = bindlessReflection.createPushConstantRanges();

            m_bindlessOffscreenPipeline.createWithLayouts(
                device, m_offscreenRenderPass, bindlessLayouts,
                {m_offscreenWidth, m_offscreenHeight}, pcRanges, VK_POLYGON_MODE_FILL,
                "shaders/triangle_vert_bindless.spv",
                "shaders/triangle_frag_bindless.spv");

            m_bindlessWireframePipeline.createWithLayouts(
                device, m_offscreenRenderPass, bindlessLayouts,
                {m_offscreenWidth, m_offscreenHeight}, pcRanges, VK_POLYGON_MODE_LINE,
                "shaders/triangle_vert_bindless.spv",
                "shaders/triangle_frag_bindless.spv");
        }
    }

    // 4. Rebuild main pipeline
    m_pipeline.cleanup(device);
    m_pipeline.create(device, m_renderPass.get(), m_swapChain.getExtent(), m_layoutCache);
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
    m_lastDrawCallCount = 0;
    m_lastTriangleCount = 0;

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

    VkDescriptorSet frameSet = m_descriptor.getPerFrameSet(m_currentFrame);

    // --- Draw grid + sky (before scene objects) ---
    if (m_gridPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_gridPipelineLayout, 0, 1, &frameSet, 0, nullptr);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    // Build frustum for CPU-side culling
    Frustum frustum;
    if (m_camera) {
        float aspect = m_offscreenWidth / static_cast<float>(m_offscreenHeight);
        glm::mat4 vp = m_camera->getProjMatrix(aspect) * m_camera->getViewMatrix();
        frustum.update(vp);
    }

    // --- Bindless path: bind pipeline + set 1 once for all objects ---
    if (m_bindlessEnabled && m_bindlessOffscreenPipeline.getPipeline() != VK_NULL_HANDLE) {
        VkPipelineLayout bindlessLayout = m_bindlessOffscreenPipeline.getPipelineLayout();

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_bindlessOffscreenPipeline.getPipeline());

        // Bind set 0 (per-frame)
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                bindlessLayout, 0, 1, &frameSet, 0, nullptr);

        // Bind set 1 (bindless) ONCE for the entire pass
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                bindlessLayout, 1, 1, &m_bindlessSet, 0, nullptr);

        scene.traverseNodes([&](Node* node) {
            if (node->nodeType == NodeType::DirectionalLight) return;

            AABB localAABB;
            if (!node->meshPath.empty()) {
                auto* mesh = m_assetManager.loadMesh(node->meshPath);
                if (mesh) {
                    localAABB.min = mesh->boundsMin;
                    localAABB.max = mesh->boundsMax;
                }
            } else if (node->meshType != MeshType::None) {
                localAABB = m_meshLibrary.getAABB(node->meshType);
            } else {
                return;
            }

            if (!frustum.isVisible(localAABB, node->getWorldMatrix()))
                return;

            const MaterialInstance* mat = nullptr;
            if (!node->materialPath.empty())
                mat = m_assetManager.loadMaterial(node->materialPath);

            PushConstantData pc{};
            pc.model = glm::transpose(node->getWorldMatrix());
            pc.highlighted = 0;
            pc.materialIndex = mat ? mat->bindlessIndex : m_defaultBindlessMaterialIndex;

            vkCmdPushConstants(commandBuffer, bindlessLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(PushConstantData), &pc);

            if (!node->meshPath.empty()) {
                auto* mesh = m_assetManager.loadMesh(node->meshPath);
                if (mesh) {
                    VkBuffer buffers[] = {mesh->vertexBuffer};
                    VkDeviceSize offsets[] = {0};
                    vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);
                    vkCmdBindIndexBuffer(commandBuffer, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(commandBuffer, mesh->indexCount, 1, 0, 0, 0);
                    m_lastDrawCallCount++;
                    m_lastTriangleCount += mesh->indexCount / 3;
                }
            } else if (node->meshType != MeshType::None) {
                m_meshLibrary.bind(commandBuffer, node->meshType);
                uint32_t idxCount = m_meshLibrary.getIndexCount(node->meshType);
                vkCmdDrawIndexed(commandBuffer, idxCount, 1, 0, 0, 0);
                m_lastDrawCallCount++;
                m_lastTriangleCount += idxCount / 3;
            }
        });
    } else {
        // --- Non-bindless path (fallback / Android) ---
        scene.traverseNodes([&](Node* node) {
            if (node->nodeType == NodeType::DirectionalLight) return;

            AABB localAABB;
            if (!node->meshPath.empty()) {
                auto* mesh = m_assetManager.loadMesh(node->meshPath);
                if (mesh) {
                    localAABB.min = mesh->boundsMin;
                    localAABB.max = mesh->boundsMax;
                }
            } else if (node->meshType != MeshType::None) {
                localAABB = m_meshLibrary.getAABB(node->meshType);
            } else {
                return;
            }

            if (!frustum.isVisible(localAABB, node->getWorldMatrix()))
                return;

            const MaterialInstance* mat = nullptr;
            if (!node->materialPath.empty())
                mat = m_assetManager.loadMaterial(node->materialPath);

            VkPipeline shaderPipeline;
            VkPipelineLayout pipelineLayout;
            VkDescriptorSet matSet;

            if (mat && mat->shader &&
                mat->shader->pipeline.getPipeline() != VK_NULL_HANDLE &&
                mat->descriptorSet != VK_NULL_HANDLE) {
                shaderPipeline = mat->shader->pipeline.getPipeline();
                pipelineLayout = mat->shader->pipeline.getPipelineLayout();
                matSet = mat->descriptorSet;
            } else {
                shaderPipeline = m_offscreenPipeline.getPipeline();
                pipelineLayout = m_offscreenPipeline.getPipelineLayout();
                matSet = m_defaultMaterialSet;
            }

            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shaderPipeline);

            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 0, 1, &frameSet, 0, nullptr);

            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 1, 1, &matSet, 0, nullptr);

            PushConstantData pc{};
            pc.model = glm::transpose(node->getWorldMatrix());
            pc.highlighted = 0;
            vkCmdPushConstants(commandBuffer, pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0, sizeof(PushConstantData), &pc);

            if (!node->meshPath.empty()) {
                auto* mesh = m_assetManager.loadMesh(node->meshPath);
                if (mesh) {
                    VkBuffer buffers[] = {mesh->vertexBuffer};
                    VkDeviceSize offsets[] = {0};
                    vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);
                    vkCmdBindIndexBuffer(commandBuffer, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(commandBuffer, mesh->indexCount, 1, 0, 0, 0);
                    m_lastDrawCallCount++;
                    m_lastTriangleCount += mesh->indexCount / 3;
                }
            } else if (node->meshType != MeshType::None) {
                m_meshLibrary.bind(commandBuffer, node->meshType);
                uint32_t idxCount = m_meshLibrary.getIndexCount(node->meshType);
                vkCmdDrawIndexed(commandBuffer, idxCount, 1, 0, 0, 0);
                m_lastDrawCallCount++;
                m_lastTriangleCount += idxCount / 3;
            }
        });
    }

    // Draw wireframe outline for selected node
    Node* selected = scene.getSelectedNode();
    if (selected && (selected->meshType != MeshType::None || !selected->meshPath.empty())) {
        // Choose wireframe pipeline based on bindless mode
        Pipeline& wfPipe = (m_bindlessEnabled && m_bindlessWireframePipeline.getPipeline() != VK_NULL_HANDLE)
                           ? m_bindlessWireframePipeline : m_wireframePipeline;
        VkPipelineLayout wfLayout = wfPipe.getPipelineLayout();

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wfPipe.getPipeline());

        // Re-bind set 0 after pipeline change
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                wfLayout, 0, 1, &frameSet, 0, nullptr);

        if (m_bindlessEnabled && m_bindlessWireframePipeline.getPipeline() != VK_NULL_HANDLE) {
            // Bind bindless set 1
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    wfLayout, 1, 1, &m_bindlessSet, 0, nullptr);
        } else {
            // Bind wireframe material set 1 (orange baseColor)
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    wfLayout, 1, 1, &m_wireframeMaterialSet, 0, nullptr);
        }

        PushConstantData pc{};
        pc.model = glm::transpose(selected->getWorldMatrix());
        pc.highlighted = 1;
        pc.materialIndex = m_wireframeBindlessMaterialIndex;
        VkShaderStageFlags wfPcStages = (m_bindlessEnabled && m_bindlessWireframePipeline.getPipeline() != VK_NULL_HANDLE)
            ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
            : VK_SHADER_STAGE_VERTEX_BIT;
        vkCmdPushConstants(commandBuffer, wfLayout, wfPcStages,
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

        Pipeline& wfPipe = (m_bindlessEnabled && m_bindlessWireframePipeline.getPipeline() != VK_NULL_HANDLE)
                           ? m_bindlessWireframePipeline : m_wireframePipeline;
        VkPipelineLayout wfLayout = wfPipe.getPipelineLayout();

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wfPipe.getPipeline());

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                wfLayout, 0, 1, &frameSet, 0, nullptr);

        if (m_bindlessEnabled && m_bindlessWireframePipeline.getPipeline() != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    wfLayout, 1, 1, &m_bindlessSet, 0, nullptr);
        } else {
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    wfLayout, 1, 1, &m_wireframeMaterialSet, 0, nullptr);
        }

        // Light gizmo: sphere at position + single arrow showing direction
        glm::vec3 pos = node->transform.position;
        glm::vec3 dir = node->getLightDirection();

        VkShaderStageFlags gizmoPcStages = (m_bindlessEnabled && m_bindlessWireframePipeline.getPipeline() != VK_NULL_HANDLE)
            ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
            : VK_SHADER_STAGE_VERTEX_BIT;

        PushConstantData pc{};
        pc.highlighted = 1;
        pc.materialIndex = m_wireframeBindlessMaterialIndex;

        // Sphere at light position
        pc.model = glm::transpose(glm::scale(glm::translate(glm::mat4(1.0f), pos), glm::vec3(0.15f)));
        vkCmdPushConstants(commandBuffer, wfLayout, gizmoPcStages,
            0, sizeof(PushConstantData), &pc);
        m_meshLibrary.bind(commandBuffer, MeshType::Sphere);
        vkCmdDrawIndexed(commandBuffer, m_meshLibrary.getIndexCount(MeshType::Sphere), 1, 0, 0, 0);

        // Arrow from sphere center along light direction
        float arrowLen = 0.6f;
        glm::vec3 arrowCenter = pos + dir * (arrowLen * 0.5f);

        // Build rotation from (0,1,0) to dir using quaternion
        glm::vec3 defaultUp = glm::vec3(0, 1, 0);
        glm::vec3 axis = glm::cross(defaultUp, dir);
        float axisLen = glm::length(axis);
        glm::mat4 rotMat(1.0f);
        if (axisLen > 0.0001f) {
            axis /= axisLen;
            float angle = std::acos(glm::clamp(glm::dot(defaultUp, dir), -1.0f, 1.0f));
            rotMat = glm::rotate(glm::mat4(1.0f), angle, axis);
        } else if (glm::dot(defaultUp, dir) < 0.0f) {
            rotMat = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1, 0, 0));
        }

        pc.model = glm::transpose(glm::translate(glm::mat4(1.0f), arrowCenter)
                 * rotMat
                 * glm::scale(glm::mat4(1.0f), glm::vec3(0.03f, arrowLen, 0.03f)));
        vkCmdPushConstants(commandBuffer, wfLayout, gizmoPcStages,
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
        ubo.view = glm::transpose(m_camera->getViewMatrix());
        ubo.proj = glm::transpose(m_camera->getProjMatrix(aspect));
    } else {
        ubo.view = glm::lookAt(glm::vec3(2,2,2), glm::vec3(0,0,0), glm::vec3(0,1,0));
        ubo.proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
        ubo.proj[1][1] *= -1;
        ubo.view = glm::transpose(ubo.view);
        ubo.proj = glm::transpose(ubo.proj);
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
    // 等待窗口恢复非零尺寸
    SDL_Window* nativeWindow = m_window->getNativeWindow();
    int width = 0, height = 0;
    SDL_Vulkan_GetDrawableSize(nativeWindow, &width, &height);
    while (width == 0 || height == 0)
    {
        SDL_Vulkan_GetDrawableSize(nativeWindow, &width, &height);
        SDL_WaitEvent(nullptr);
    }

    vkDeviceWaitIdle(m_context.getDevice());

    m_swapChain.cleanup(m_context.getDevice());

    m_swapChain.create(m_context, nativeWindow);
    m_swapChain.createFramebuffers(m_context.getDevice(), m_renderPass.get());

    if (m_swapChainRecreatedCb)
        m_swapChainRecreatedCb();
}

} // namespace QymEngine
