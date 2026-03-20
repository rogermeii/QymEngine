#include "renderer/Renderer.h"
#include "core/Window.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
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
    m_renderPass.create(m_context.getDevice(), m_swapChain.getImageFormat());
    m_descriptor.createLayout(m_context.getDevice());
    m_pipeline.create(m_context.getDevice(), m_renderPass.get(),
                      m_descriptor.getLayout(), m_swapChain.getExtent());
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

    return true;
}

void Renderer::drawScene()
{
    VkCommandBuffer cmdBuf = m_commandManager.getBuffer(m_currentFrame);
    vkResetCommandBuffer(cmdBuf, 0);
    recordCommandBuffer(cmdBuf, m_currentImageIndex);
}

void Renderer::endFrame()
{
    VkDevice device = m_context.getDevice();
    VkCommandBuffer cmdBuf = m_commandManager.getBuffer(m_currentFrame);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = { m_swapChain.getImageAvailableSemaphore(m_currentFrame) };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    VkSemaphore signalSemaphores[] = { m_swapChain.getRenderFinishedSemaphore(m_currentFrame) };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(m_context.getGraphicsQueue(), 1, &submitInfo,
                      m_swapChain.getInFlightFence(m_currentFrame)) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = { m_swapChain.getSwapChain() };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &m_currentImageIndex;
    presentInfo.pResults = nullptr;

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

    m_swapChain.cleanup(device);
    m_texture.cleanup(device);
    m_buffer.cleanup(device, MAX_FRAMES_IN_FLIGHT);
    m_descriptor.cleanup(device);
    m_pipeline.cleanup(device);
    m_renderPass.cleanup(device);
    m_swapChain.cleanupSyncObjects(device, MAX_FRAMES_IN_FLIGHT);
    m_commandManager.cleanup(device);
    m_context.shutdown();
}

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = 0;
    beginInfo.pInheritanceInfo = nullptr;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
        throw std::runtime_error("failed to begin recording command buffer!");

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass.get();
    renderPassInfo.framebuffer = m_swapChain.getFramebuffers()[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = m_swapChain.getExtent();

    VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.getPipeline());

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_swapChain.getExtent().width);
    viewport.height = static_cast<float>(m_swapChain.getExtent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_swapChain.getExtent();
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    VkBuffer vertexBuffers[] = { m_buffer.getVertexBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdBindIndexBuffer(commandBuffer, m_buffer.getIndexBuffer(), 0, VK_INDEX_TYPE_UINT16);

    VkDescriptorSet descriptorSet = m_descriptor.getSet(m_currentFrame);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipeline.getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);

    vkCmdDrawIndexed(commandBuffer, m_buffer.getIndexCount(), 1, 0, 0, 0);

    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
        throw std::runtime_error("failed to record command buffer!");
}

void Renderer::updateUniformBuffer(uint32_t currentImage)
{
    static auto startTime = std::chrono::high_resolution_clock::now();

    auto currentTime = std::chrono::high_resolution_clock::now();
    float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

    UniformBufferObject ubo{};
    ubo.model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.proj = glm::perspective(glm::radians(45.0f),
        m_swapChain.getExtent().width / (float)m_swapChain.getExtent().height, 0.1f, 10.0f);
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
}

} // namespace QymEngine
