#include "ModelPreview.h"
#include "renderer/Renderer.h"

#include <imgui_impl_vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace QymEngine {

void ModelPreview::init(Renderer& renderer)
{
    createResources(renderer);
}

void ModelPreview::shutdown(VkDevice device)
{
    if (m_descriptorSet != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_descriptorSet);
        m_descriptorSet = VK_NULL_HANDLE;
    }

    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_depthView, nullptr);
        m_depthView = VK_NULL_HANDLE;
    }
    if (m_depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
    }
    if (m_depthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_depthMemory, nullptr);
        m_depthMemory = VK_NULL_HANDLE;
    }
    if (m_colorView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_colorView, nullptr);
        m_colorView = VK_NULL_HANDLE;
    }
    if (m_colorImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_colorImage, nullptr);
        m_colorImage = VK_NULL_HANDLE;
    }
    if (m_colorMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_colorMemory, nullptr);
        m_colorMemory = VK_NULL_HANDLE;
    }
    if (m_uboBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_uboBuffer, nullptr);
        m_uboBuffer = VK_NULL_HANDLE;
    }
    if (m_uboMemory != VK_NULL_HANDLE) {
        vkUnmapMemory(device, m_uboMemory);
        vkFreeMemory(device, m_uboMemory, nullptr);
        m_uboMemory = VK_NULL_HANDLE;
        m_uboMapped = nullptr;
    }

    m_initialized = false;
}

void ModelPreview::createResources(Renderer& renderer)
{
    if (m_initialized) return;

    if (renderer.getOffscreenRenderPass() == VK_NULL_HANDLE)
        return;

    VkDevice device = renderer.getContext().getDevice();
    VkFormat colorFormat = renderer.getSwapChain().getImageFormat();

    // --- Color image ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width  = PREVIEW_SIZE;
        imageInfo.extent.height = PREVIEW_SIZE;
        imageInfo.extent.depth  = 1;
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.format        = colorFormat;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &imageInfo, nullptr, &m_colorImage) != VK_SUCCESS)
            throw std::runtime_error("failed to create model preview color image!");

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, m_colorImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = renderer.getContext().findMemoryType(
            memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &m_colorMemory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate model preview color memory!");

        vkBindImageMemory(device, m_colorImage, m_colorMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                           = m_colorImage;
        viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                          = colorFormat;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &m_colorView) != VK_SUCCESS)
            throw std::runtime_error("failed to create model preview color image view!");
    }

    // --- Depth image ---
    {
        VkImageCreateInfo depthInfo{};
        depthInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depthInfo.imageType     = VK_IMAGE_TYPE_2D;
        depthInfo.extent        = {PREVIEW_SIZE, PREVIEW_SIZE, 1};
        depthInfo.mipLevels     = 1;
        depthInfo.arrayLayers   = 1;
        depthInfo.format        = VK_FORMAT_D32_SFLOAT;
        depthInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depthInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        depthInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &depthInfo, nullptr, &m_depthImage) != VK_SUCCESS)
            throw std::runtime_error("failed to create model preview depth image!");

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, m_depthImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = renderer.getContext().findMemoryType(
            memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &m_depthMemory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate model preview depth memory!");

        vkBindImageMemory(device, m_depthImage, m_depthMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                           = m_depthImage;
        viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                          = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &m_depthView) != VK_SUCCESS)
            throw std::runtime_error("failed to create model preview depth image view!");
    }

    // --- Sampler ---
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

        if (vkCreateSampler(device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create model preview sampler!");
    }

    // --- Framebuffer (reuses offscreen render pass) ---
    {
        std::array<VkImageView, 2> attachments = {m_colorView, m_depthView};

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = renderer.getOffscreenRenderPass();
        fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        fbInfo.pAttachments    = attachments.data();
        fbInfo.width           = PREVIEW_SIZE;
        fbInfo.height          = PREVIEW_SIZE;
        fbInfo.layers          = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_framebuffer) != VK_SUCCESS)
            throw std::runtime_error("failed to create model preview framebuffer!");
    }

    m_descriptorSet = ImGui_ImplVulkan_AddTexture(
        m_sampler, m_colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // --- Dedicated UBO buffer for preview camera ---
    {
        VkDeviceSize bufferSize = sizeof(UniformBufferObject);
        Buffer::createBuffer(renderer.getContext(), bufferSize,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m_uboBuffer, m_uboMemory);
        vkMapMemory(device, m_uboMemory, 0, bufferSize, 0, &m_uboMapped);

        VkDescriptorSetLayout uboLayout = renderer.getDescriptor().getUboLayout();
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = renderer.getDescriptor().getPool();
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &uboLayout;

        if (vkAllocateDescriptorSets(device, &allocInfo, &m_uboDescriptorSet) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate model preview UBO descriptor set!");

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uboBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range  = sizeof(UniformBufferObject);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet          = m_uboDescriptorSet;
        descriptorWrite.dstBinding      = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo     = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    m_initialized = true;
}

void ModelPreview::writePreviewUbo(const glm::vec3& boundsMin, const glm::vec3& boundsMax)
{
    glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
    float radius = glm::length(boundsMax - boundsMin) * 0.5f;
    if (radius < 0.001f) radius = 0.5f;

    float fovY = glm::radians(45.0f);
    float distance = (radius / std::tan(fovY * 0.5f)) * 1.3f;

    glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.7f, 1.0f));
    glm::vec3 eye = center + dir * distance;

    UniformBufferObject previewUbo{};
    previewUbo.view = glm::lookAt(eye, center, glm::vec3(0, 1, 0));
    previewUbo.proj = glm::perspective(fovY, 1.0f, 0.01f, distance * 3.0f);
    previewUbo.proj[1][1] *= -1;  // Vulkan Y-flip
    previewUbo.lightDir = glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f));
    previewUbo.lightColor = glm::vec3(1.0f, 1.0f, 1.0f);
    previewUbo.ambientColor = glm::vec3(0.2f, 0.2f, 0.2f);
    previewUbo.cameraPos = eye;
    memcpy(m_uboMapped, &previewUbo, sizeof(UniformBufferObject));
}

void ModelPreview::beginPreviewPass(Renderer& renderer)
{
    VkCommandBuffer cmd = renderer.getCurrentCommandBuffer();

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = renderer.getOffscreenRenderPass();
    rpInfo.framebuffer       = m_framebuffer;
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {PREVIEW_SIZE, PREVIEW_SIZE};

    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{0.15f, 0.15f, 0.18f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    rpInfo.clearValueCount = static_cast<uint32_t>(clears.size());
    rpInfo.pClearValues    = clears.data();

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0, 0, static_cast<float>(PREVIEW_SIZE), static_cast<float>(PREVIEW_SIZE), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {PREVIEW_SIZE, PREVIEW_SIZE}};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      renderer.getOffscreenPipeline().getPipeline());

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        renderer.getOffscreenPipeline().getPipelineLayout(), 0, 1, &m_uboDescriptorSet, 0, nullptr);

    VkDescriptorSet texSet = renderer.getDefaultMaterialTexSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        renderer.getOffscreenPipeline().getPipelineLayout(), 1, 1, &texSet, 0, nullptr);

    PushConstantData pc{};
    pc.model = glm::mat4(1.0f);
    pc.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    pc.metallic = 0.0f;
    pc.roughness = 0.5f;
    pc.highlighted = 0;
    vkCmdPushConstants(cmd, renderer.getOffscreenPipeline().getPipelineLayout(),
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(PushConstantData), &pc);
}

void ModelPreview::renderBuiltIn(Renderer& renderer, MeshType type)
{
    if (!m_initialized) createResources(renderer);
    if (!m_initialized) return;

    writePreviewUbo(glm::vec3(-0.5f), glm::vec3(0.5f));
    beginPreviewPass(renderer);

    VkCommandBuffer cmd = renderer.getCurrentCommandBuffer();
    renderer.getMeshLibrary().bind(cmd, type);
    vkCmdDrawIndexed(cmd, renderer.getMeshLibrary().getIndexCount(type), 1, 0, 0, 0);

    vkCmdEndRenderPass(cmd);
}

void ModelPreview::renderMesh(Renderer& renderer, const MeshAsset* mesh)
{
    if (!mesh) return;
    if (!m_initialized) createResources(renderer);
    if (!m_initialized) return;

    writePreviewUbo(mesh->boundsMin, mesh->boundsMax);
    beginPreviewPass(renderer);

    VkCommandBuffer cmd = renderer.getCurrentCommandBuffer();
    VkBuffer vertBuffers[] = {mesh->vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);

    vkCmdEndRenderPass(cmd);
}

} // namespace QymEngine
