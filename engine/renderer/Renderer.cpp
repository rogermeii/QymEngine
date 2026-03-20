#include "renderer/Renderer.h"
#include "core/Window.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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

    m_descriptor.createLayout(m_context.getDevice());

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstantData);
    std::vector<VkPushConstantRange> pushConstantRanges = { pushConstantRange };

    m_pipeline.create(m_context.getDevice(), m_renderPass.get(),
                      m_descriptor.getLayout(), m_swapChain.getExtent(),
                      pushConstantRanges);

    // Swapchain framebuffers — still created, used by ImGuiLayer's render pass
    // (ImGuiLayer creates its own framebuffers but SwapChain also needs them for
    //  the old render pass reference; we keep them to avoid breaking the cleanup path).
    m_swapChain.createFramebuffers(m_context.getDevice(), m_renderPass.get());

    m_texture.createTextureImage(m_context, m_commandManager);
    m_texture.createTextureImageView(m_context.getDevice());
    m_texture.createTextureSampler(m_context);

    m_buffer.createVertexBuffer(m_context, m_commandManager);
    m_buffer.createIndexBuffer(m_context, m_commandManager);
    m_buffer.createUniformBuffers(m_context, MAX_FRAMES_IN_FLIGHT);

    m_descriptor.createPool(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT);
    m_descriptor.createSets(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT,
                            m_buffer.getUniformBuffers(),
                            m_texture.getImageView(), m_texture.getSampler());

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
    // If offscreen is set up, render to offscreen. Otherwise no-op.
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

void Renderer::shutdown()
{
    vkDeviceWaitIdle(m_context.getDevice());

    VkDevice device = m_context.getDevice();

    destroyOffscreen();

    m_swapChain.cleanup(device);
    m_texture.cleanup(device);
    m_buffer.cleanup(device, MAX_FRAMES_IN_FLIGHT);
    m_descriptor.cleanup(device);
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

    // --- 2. Create image view ---
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

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        // Dependencies to ensure proper synchronization
        VkSubpassDependency dependencies[2] = {};

        // Before render pass: wait for previous fragment shader reads to finish
        dependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass      = 0;
        dependencies[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        // After render pass: make sure color writes are visible to fragment shader
        dependencies[1].srcSubpass      = 0;
        dependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments    = &colorAttachment;
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

        m_offscreenPipeline.create(device, m_offscreenRenderPass,
                                   m_descriptor.getLayout(), {width, height},
                                   pcRanges);
    }

    // --- 5. Create framebuffer ---
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_offscreenRenderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments    = &m_offscreenImageView;
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

    VkClearValue clearColor = {{{0.1f, 0.1f, 0.1f, 1.0f}}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues    = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_offscreenPipeline.getPipeline());

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

    VkBuffer vertexBuffers[] = { m_buffer.getVertexBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdBindIndexBuffer(commandBuffer, m_buffer.getIndexBuffer(), 0, VK_INDEX_TYPE_UINT16);

    VkDescriptorSet descriptorSet = m_descriptor.getSet(m_currentFrame);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_offscreenPipeline.getPipelineLayout(), 0, 1,
                            &descriptorSet, 0, nullptr);

    // Render each scene node with its own model matrix via push constants
    scene.traverseNodes([&](Node* node) {
        PushConstantData pc{};
        pc.model = node->getWorldMatrix();
        pc.highlighted = (node == scene.getSelectedNode()) ? 1 : 0;
        vkCmdPushConstants(commandBuffer, m_offscreenPipeline.getPipelineLayout(),
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(PushConstantData), &pc);
        vkCmdDrawIndexed(commandBuffer, m_buffer.getIndexCount(), 1, 0, 0, 0);
    });

    vkCmdEndRenderPass(commandBuffer);
}

// ---------------------------------------------------------------------------
// Old recordCommandBuffer — no longer used for swapchain scene rendering.
// ---------------------------------------------------------------------------
void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex)
{
    // No longer used. Scene rendering goes through drawSceneToOffscreen().
}

void Renderer::updateUniformBuffer(uint32_t currentImage)
{
    UniformBufferObject ubo{};
    ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    float aspect = (m_offscreenWidth > 0)
        ? m_offscreenWidth / static_cast<float>(m_offscreenHeight)
        : 800.0f / 600.0f;
    ubo.proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
    ubo.proj[1][1] *= -1;

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
