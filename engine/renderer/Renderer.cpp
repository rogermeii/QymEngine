#include "renderer/Renderer.h"
#include "renderer/VkDispatch.h"
#include "asset/ShaderBundle.h"
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

namespace {
glm::mat4 toShaderMatrix(const glm::mat4& m)
{
    // OpenGL/GLES 的模型矩阵都走显式 GLSL 修正路径，直接上传 GLM 原始矩阵。
    if (vkIsOpenGLBackend() || vkIsGLESBackend())
        return m;
    return glm::transpose(m);
}
} // namespace

// 根据当前后端选择 ShaderBundle 变体名
// Vulkan: "default" / "bindless"
// D3D12:  "default_dxil" / "bindless_dxil"
// D3D11:  "default_dxbc" / "bindless_dxbc"
static std::string shaderVariant(const std::string& base) {
    if (vkIsD3D12Backend())
        return base + "_dxil";
    if (vkIsD3D11Backend())
        return base + "_dxbc";
    if (vkIsOpenGLBackend() || vkIsGLESBackend())
        return base + "_glsl";
    return base;
}

static std::string shaderBundlePath(const char* bundleBaseName) {
#ifdef __ANDROID__
    return std::string("shaders/") + bundleBaseName + ".shaderbundle";
#else
    return std::string(ASSETS_DIR) + "/shaders/" + bundleBaseName + ".shaderbundle";
#endif
}

static void createFullscreenBundlePipeline(
    VkDevice device,
    VkRenderPass renderPass,
    const std::vector<VkDescriptorSetLayout>& setLayouts,
    const std::string& bundlePath,
    const std::string& variant,
    bool blendEnable,
    bool depthTestEnable,
    bool depthWriteEnable,
    VkPipeline& outPipeline,
    VkPipelineLayout& outLayout,
    const char* debugName,
    const VkPushConstantRange* pushConstantRange = nullptr,
    const VkPipelineColorBlendAttachmentState* customBlendState = nullptr)
{
    ShaderBundle bundle;
    if (!bundle.load(bundlePath) || !bundle.hasVariant(variant))
        throw std::runtime_error(std::string("Failed to load ") + debugName + " variant: " + variant);

    auto createShaderModule = [&](const std::vector<char>& code) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS)
            throw std::runtime_error(std::string("failed to create ") + debugName + " shader module!");
        return mod;
    };

    VkShaderModule vert = createShaderModule(bundle.getVertSpv(variant));
    VkShaderModule frag = createShaderModule(bundle.getFragSpv(variant));

    if (outPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, outPipeline, nullptr);
        outPipeline = VK_NULL_HANDLE;
    }
    if (outLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, outLayout, nullptr);
        outLayout = VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    if (customBlendState) {
        colorBlendAttachment = *customBlendState;
    } else {
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = blendEnable ? VK_TRUE : VK_FALSE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = depthTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = depthWriteEnable ? VK_TRUE : VK_FALSE;
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

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = pushConstantRange ? 1 : 0;
    layoutInfo.pPushConstantRanges = pushConstantRange;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &outLayout) != VK_SUCCESS)
        throw std::runtime_error(std::string("failed to create ") + debugName + " pipeline layout!");

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
    pipelineInfo.layout = outLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline) != VK_SUCCESS)
        throw std::runtime_error(std::string("failed to create ") + debugName + " pipeline!");

    vkDestroyShaderModule(device, frag, nullptr);
    vkDestroyShaderModule(device, vert, nullptr);
}

void Renderer::init(Window& window)
{
    m_window = &window;

    // Register resize callback
    m_window->setFramebufferResizeCallback([this](int, int) {
        m_framebufferResized = true;
    });

    SDL_Window* nativeWindow = m_window->getNativeWindow();

    // Init order follows dependency chain
    SDL_Log("[Renderer] init: context.init...");
    m_context.init(nativeWindow);
    SDL_Log("[Renderer] init: swapChain.create...");
    m_swapChain.create(m_context, nativeWindow);
    SDL_Log("[Renderer] init: swapChain created");
    m_commandManager.createPool(m_context);

    // The original scene RenderPass (used for swapchain). Kept for init compatibility.
    m_renderPass.create(m_context.getDevice(), m_swapChain.getImageFormat());

    // Register per-frame layouts in cache
    {
        VkDescriptorSetLayoutBinding uboBinding{};
        uboBinding.binding = 0;
        uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboBinding.descriptorCount = 1;
        uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        uboBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutBinding shadowBinding{};
        shadowBinding.binding = 1;
        shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBinding.descriptorCount = 1;
        shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        shadowBinding.pImmutableSamplers = nullptr;

        // IBL bindings (binding 2/3/4)
        VkDescriptorSetLayoutBinding irradianceBinding{};
        irradianceBinding.binding = 2;
        irradianceBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        irradianceBinding.descriptorCount = 1;
        irradianceBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        irradianceBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutBinding prefilteredBinding{};
        prefilteredBinding.binding = 3;
        prefilteredBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        prefilteredBinding.descriptorCount = 1;
        prefilteredBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        prefilteredBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutBinding brdfLutBinding{};
        brdfLutBinding.binding = 4;
        brdfLutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        brdfLutBinding.descriptorCount = 1;
        brdfLutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        brdfLutBinding.pImmutableSamplers = nullptr;

        m_perFrameLayout = m_layoutCache.getOrCreate(m_context.getDevice(),
            {uboBinding, shadowBinding, irradianceBinding, prefilteredBinding, brdfLutBinding});
        m_frameOnlyLayout = m_layoutCache.getOrCreate(m_context.getDevice(), {uboBinding});
    }

    // Create main pipeline from ShaderBundle
    {
        std::string triPath = std::string(ASSETS_DIR) + "/shaders/Triangle.shaderbundle";
        ShaderBundle triBundle;
        std::string var = shaderVariant("default");
        if (!triBundle.load(triPath) || !triBundle.hasVariant(var))
            throw std::runtime_error("Failed to load Triangle.shaderbundle variant: " + var);
        m_pipeline.createFromMemory(m_context.getDevice(), m_renderPass.get(),
            m_swapChain.getExtent(), m_layoutCache, VK_POLYGON_MODE_FILL,
            triBundle.getVertSpv(var), triBundle.getFragSpv(var),
            triBundle.getReflectJson(var), m_perFrameLayout);
    }

    // Swapchain framebuffers
    m_swapChain.createFramebuffers(m_context.getDevice(), m_renderPass.get());

    m_texture.createTextureImage(m_context, m_commandManager);
    m_texture.createTextureImageView(m_context.getDevice());
    m_texture.createTextureSampler(m_context);

    m_meshLibrary.init(m_context, m_commandManager);
    m_buffer.createUniformBuffers(m_context, MAX_FRAMES_IN_FLIGHT);

    // Create descriptor pool
    m_descriptor.createPool(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT, 100);

    // Create shadow map resources (before per-frame sets, they need shadow sampler)
    createShadowResources();

    // Create per-frame UBO descriptor sets (set 0)
    m_descriptor.createPerFrameSets(m_context.getDevice(), MAX_FRAMES_IN_FLIGHT,
                                     m_perFrameLayout,
                                     m_buffer.getUniformBuffers());

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        m_frameOnlySets[i] = m_descriptor.allocateSet(m_context.getDevice(), m_frameOnlyLayout);

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_buffer.getUniformBuffers()[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_frameOnlySets[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(m_context.getDevice(), 1, &write, 0, nullptr);
    }

    // Write shadow map sampler into per-frame descriptor sets (binding 1)
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.sampler = m_shadowSampler;
        shadowInfo.imageView = m_shadowImageView;
        shadowInfo.imageLayout = vkIsGLESBackend()
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptor.getPerFrameSet(i);
        write.dstBinding = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &shadowInfo;
        vkUpdateDescriptorSets(m_context.getDevice(), 1, &write, 0, nullptr);
    }

    // Create fallback textures for the material system
    createFallbackTextures();

    // Create IBL fallback textures (1x1 black cubemap + 2D for bindings 2/3/4)
    createIBLFallbackTextures();

    // Write IBL fallback descriptors into per-frame sets (binding 2/3/4)
    writeIBLDescriptors();

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
    static int s_drawFrame = 0;
    if (s_drawFrame < 3) {
        std::cerr << "[Renderer] drawScene frame=" << s_drawFrame
                  << " offRP=" << (m_offscreenRenderPass != VK_NULL_HANDLE)
                  << " offFB=" << (m_offscreenFramebuffer != VK_NULL_HANDLE) << std::endl;
    }
    s_drawFrame++;

    m_activeScene = &scene;
    if (m_offscreenRenderPass != VK_NULL_HANDLE && m_offscreenFramebuffer != VK_NULL_HANDLE)
    {
        VkCommandBuffer cmdBuf = m_commandManager.getBuffer(m_currentFrame);
        // Shadow pass (before main scene pass)
        if (m_shadowRenderPass != VK_NULL_HANDLE && m_shadowFramebuffer != VK_NULL_HANDLE
            && m_shadowVkPipeline != VK_NULL_HANDLE) {
            renderShadowPass(cmdBuf, scene);
        }
        drawSceneToOffscreen(cmdBuf, scene);

        // 后处理：在离屏渲染完成后执行
        auto& ppSettings = scene.getPostProcessSettings();
        float nearPlane = m_camera ? m_camera->nearPlane : 0.1f;
        float farPlane = m_camera ? m_camera->farPlane : 100.0f;
        m_postProcess.execute(cmdBuf, m_offscreenImageView,
                              m_offscreenDepthImageView, nearPlane, farPlane,
                              ppSettings);
        m_displayImage = m_postProcess.getFinalImage(ppSettings);
        m_displayImageView = m_postProcess.getFinalImageView(ppSettings);
    }
}

void Renderer::blitToSwapchain()
{
    if (!isOffscreenReady()) return;

    // 使用后处理输出图像；若尚未执行后处理则回退到离屏图像
    VkImage srcImage = (m_displayImage != VK_NULL_HANDLE) ? m_displayImage : m_offscreenImage;

    VkCommandBuffer cmd = m_commandManager.getBuffer(m_currentFrame);
    VkImage swapImage = m_swapChain.getImages()[m_currentImageIndex];

    // Transition display image: SHADER_READ_ONLY -> TRANSFER_SRC
    VkImageMemoryBarrier offscreenBarrier{};
    offscreenBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    offscreenBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    offscreenBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    offscreenBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    offscreenBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    offscreenBarrier.image = srcImage;
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

    // Blit display -> swapchain
    VkImageBlit blitRegion{};
    blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.srcOffsets[0] = {0, 0, 0};
    blitRegion.srcOffsets[1] = {(int32_t)m_offscreenWidth, (int32_t)m_offscreenHeight, 1};
    blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.dstOffsets[0] = {0, 0, 0};
    VkExtent2D swapExtent = m_swapChain.getExtent();
    blitRegion.dstOffsets[1] = {(int32_t)swapExtent.width, (int32_t)swapExtent.height, 1};

    vkCmdBlitImage(cmd,
        srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitRegion, VK_FILTER_LINEAR);

    // Transition display image back: TRANSFER_SRC -> SHADER_READ_ONLY
    offscreenBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    offscreenBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    offscreenBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    offscreenBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &offscreenBarrier);

    // Transition swapchain: TRANSFER_DST -> PRESENT_SRC
    swapBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    swapBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    swapBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    swapBarrier.dstAccessMask = 0;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &swapBarrier);
}

VkImageView Renderer::getDisplayImageView(const Scene& scene) const {
    return m_displayImageView != VK_NULL_HANDLE ? m_displayImageView : m_offscreenImageView;
}

VkImage Renderer::getDisplayImage(const Scene& scene) const {
    return m_displayImage != VK_NULL_HANDLE ? m_displayImage : m_offscreenImage;
}

VkSampler Renderer::getDisplaySampler() const {
    return m_offscreenSampler;
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
// IBL Fallback Textures (1x1 黑色 cubemap + 1x1 黑色 2D)
// ---------------------------------------------------------------------------

void Renderer::createIBLFallbackTextures()
{
    VkDevice device = m_context.getDevice();

    // --- 1x1 黑色 cubemap (6 层) ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {1, 1, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 6;
        imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

        if (vkCreateImage(device, &imageInfo, nullptr, &m_iblFallbackCubemapImage) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL fallback cubemap image!");

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, m_iblFallbackCubemapImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = m_context.findMemoryType(memReqs.memoryTypeBits,
                                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &allocInfo, nullptr, &m_iblFallbackCubemapMemory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate IBL fallback cubemap memory!");

        vkBindImageMemory(device, m_iblFallbackCubemapImage, m_iblFallbackCubemapMemory, 0);

        // 转换布局到 SHADER_READ_ONLY
        VkCommandBuffer cmd = m_commandManager.beginSingleTimeCommands(device);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_iblFallbackCubemapImage;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        m_commandManager.endSingleTimeCommands(device, m_context.getGraphicsQueue(), cmd);

        // Cube view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_iblFallbackCubemapImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};

        if (vkCreateImageView(device, &viewInfo, nullptr, &m_iblFallbackCubemapView) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL fallback cubemap view!");
    }

    // --- 1x1 黑色 2D (BRDF LUT fallback) ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {1, 1, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R16G16_SFLOAT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &imageInfo, nullptr, &m_iblFallbackLutImage) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL fallback LUT image!");

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, m_iblFallbackLutImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = m_context.findMemoryType(memReqs.memoryTypeBits,
                                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &allocInfo, nullptr, &m_iblFallbackLutMemory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate IBL fallback LUT memory!");

        vkBindImageMemory(device, m_iblFallbackLutImage, m_iblFallbackLutMemory, 0);

        VkCommandBuffer cmd = m_commandManager.beginSingleTimeCommands(device);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_iblFallbackLutImage;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        m_commandManager.endSingleTimeCommands(device, m_context.getGraphicsQueue(), cmd);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_iblFallbackLutImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(device, &viewInfo, nullptr, &m_iblFallbackLutView) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL fallback LUT view!");
    }

    // --- 共享 sampler ---
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        if (vkCreateSampler(device, &samplerInfo, nullptr, &m_iblFallbackSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL fallback sampler!");
    }
}

void Renderer::destroyIBLFallbackTextures()
{
    VkDevice device = m_context.getDevice();
    if (m_iblFallbackSampler) { vkDestroySampler(device, m_iblFallbackSampler, nullptr); m_iblFallbackSampler = VK_NULL_HANDLE; }
    if (m_iblFallbackCubemapView) { vkDestroyImageView(device, m_iblFallbackCubemapView, nullptr); m_iblFallbackCubemapView = VK_NULL_HANDLE; }
    if (m_iblFallbackCubemapImage) { vkDestroyImage(device, m_iblFallbackCubemapImage, nullptr); m_iblFallbackCubemapImage = VK_NULL_HANDLE; }
    if (m_iblFallbackCubemapMemory) { vkFreeMemory(device, m_iblFallbackCubemapMemory, nullptr); m_iblFallbackCubemapMemory = VK_NULL_HANDLE; }
    if (m_iblFallbackLutView) { vkDestroyImageView(device, m_iblFallbackLutView, nullptr); m_iblFallbackLutView = VK_NULL_HANDLE; }
    if (m_iblFallbackLutImage) { vkDestroyImage(device, m_iblFallbackLutImage, nullptr); m_iblFallbackLutImage = VK_NULL_HANDLE; }
    if (m_iblFallbackLutMemory) { vkFreeMemory(device, m_iblFallbackLutMemory, nullptr); m_iblFallbackLutMemory = VK_NULL_HANDLE; }
}

void Renderer::writeIBLDescriptors()
{
    VkDevice device = m_context.getDevice();

    // 确定使用 IBL 生成的贴图还是 fallback
    VkImageView irradianceView = m_iblGenerator.isGenerated() ? m_iblGenerator.getIrradianceView() : m_iblFallbackCubemapView;
    VkImageView prefilteredView = m_iblGenerator.isGenerated() ? m_iblGenerator.getPrefilteredView() : m_iblFallbackCubemapView;
    VkImageView brdfLutView = m_iblGenerator.isGenerated() ? m_iblGenerator.getBrdfLutView() : m_iblFallbackLutView;
    VkSampler cubemapSampler = m_iblGenerator.isGenerated() ? m_iblGenerator.getCubemapSampler() : m_iblFallbackSampler;
    VkSampler brdfLutSampler = m_iblGenerator.isGenerated() ? m_iblGenerator.getBrdfLutSampler() : m_iblFallbackSampler;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSet perFrameSet = m_descriptor.getPerFrameSet(i);

        // binding 2: irradiance cubemap
        VkDescriptorImageInfo irradianceInfo{};
        irradianceInfo.sampler = cubemapSampler;
        irradianceInfo.imageView = irradianceView;
        irradianceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // binding 3: pre-filtered cubemap
        VkDescriptorImageInfo prefilteredInfo{};
        prefilteredInfo.sampler = cubemapSampler;
        prefilteredInfo.imageView = prefilteredView;
        prefilteredInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // binding 4: BRDF LUT
        VkDescriptorImageInfo brdfLutInfo{};
        brdfLutInfo.sampler = brdfLutSampler;
        brdfLutInfo.imageView = brdfLutView;
        brdfLutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = perFrameSet;
        writes[0].dstBinding = 2;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &irradianceInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = perFrameSet;
        writes[1].dstBinding = 3;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &prefilteredInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = perFrameSet;
        writes[2].dstBinding = 4;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].descriptorCount = 1;
        writes[2].pImageInfo = &brdfLutInfo;

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Bindless descriptor resources (PC only)
// ---------------------------------------------------------------------------

void Renderer::createBindlessResources()
{
#ifndef __ANDROID__
    if (!m_forceBindless) {
        m_bindlessEnabled = false;
        std::cout << "Bindless: disabled (use --bindless to enable)" << std::endl;
        return;
    }
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
        bindingFlags[0] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        bindingFlags[1] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
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
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_bindlessPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_bindlessSetLayout;

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

    // 销毁 IBL 生成器资源
    m_iblGenerator.destroy();
    destroyIBLFallbackTextures();

    // 销毁后处理管线资源
    m_postProcess.destroy();

    destroyOffscreen();
    destroyShadowResources();

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
    if (m_skyPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_skyPipeline, nullptr);
        m_skyPipeline = VK_NULL_HANDLE;
    }
    if (m_skyPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_skyPipelineLayout, nullptr);
        m_skyPipelineLayout = VK_NULL_HANDLE;
    }
    m_skySetLayout = VK_NULL_HANDLE;
    m_skySet = VK_NULL_HANDLE;
    m_frameOnlyLayout = VK_NULL_HANDLE;
    m_frameOnlySets = {};

    if (m_gridPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_gridPipeline, nullptr);
        m_gridPipeline = VK_NULL_HANDLE;
    }
    if (m_gridPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_gridPipelineLayout, nullptr);
        m_gridPipelineLayout = VK_NULL_HANDLE;
    }
    m_perFrameLayout = VK_NULL_HANDLE;
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
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;

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
        depthImageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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
        depthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;     // 保留深度数据供 DOF 采样
        depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;  // 渲染后转为可采样

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

        // 从 Triangle.shaderbundle 加载默认着色器
        {
            std::string triPath = std::string(ASSETS_DIR) + "/shaders/Triangle.shaderbundle";
            ShaderBundle triBundle;
            std::string var = shaderVariant("default");
            if (!triBundle.load(triPath) || !triBundle.hasVariant(var))
                throw std::runtime_error("Failed to load Triangle.shaderbundle variant: " + var);

            auto vertSpv = triBundle.getVertSpv(var);
            auto fragSpv = triBundle.getFragSpv(var);
            auto reflectJson = triBundle.getReflectJson(var);

            m_offscreenPipeline.createFromMemory(device, m_offscreenRenderPass,
                {width, height}, m_layoutCache, VK_POLYGON_MODE_FILL,
                vertSpv, fragSpv, reflectJson, m_perFrameLayout);

            m_wireframePipeline.createFromMemory(device, m_offscreenRenderPass,
                {width, height}, m_layoutCache, VK_POLYGON_MODE_LINE,
                vertSpv, fragSpv, reflectJson, m_perFrameLayout);

            // Create bindless pipelines (if enabled)
            std::string bVar = shaderVariant("bindless");
            if (m_bindlessEnabled && m_bindlessSetLayout != VK_NULL_HANDLE
                && triBundle.hasVariant(bVar)) {
                std::vector<VkDescriptorSetLayout> bindlessLayouts = {
                    m_perFrameLayout,
                    m_bindlessSetLayout
                };

                auto bVertSpv = triBundle.getVertSpv(bVar);
                auto bFragSpv = triBundle.getFragSpv(bVar);
                auto bReflectJson = triBundle.getReflectJson(bVar);

                ShaderReflectionData bindlessReflection;
                bindlessReflection.loadFromString(bReflectJson);
                auto pcRanges = bindlessReflection.createPushConstantRanges();

                m_bindlessOffscreenPipeline.createWithLayoutsFromMemory(
                    device, m_offscreenRenderPass, bindlessLayouts,
                    {width, height}, pcRanges, VK_POLYGON_MODE_FILL,
                    bVertSpv, bFragSpv, bReflectJson);

                m_bindlessWireframePipeline.createWithLayoutsFromMemory(
                    device, m_offscreenRenderPass, bindlessLayouts,
                    {width, height}, pcRanges, VK_POLYGON_MODE_LINE,
                    bVertSpv, bFragSpv, bReflectJson);
            }
        }

        // --- Create sky and grid fullscreen pipelines ---
        {
            if (m_skySetLayout == VK_NULL_HANDLE) {
                VkDescriptorSetLayoutBinding panoramaBinding{};
                panoramaBinding.binding = 0;
                panoramaBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                panoramaBinding.descriptorCount = 1;
                panoramaBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
                panoramaBinding.pImmutableSamplers = nullptr;
                m_skySetLayout = m_layoutCache.getOrCreate(device, {panoramaBinding});
            }
            if (m_skySet == VK_NULL_HANDLE) {
                m_skySet = m_descriptor.allocateSet(device, m_skySetLayout);
            }

            const TextureAsset* skyTexture = m_assetManager.loadTexture("textures/sky_panorama.png");
            if (!skyTexture)
                throw std::runtime_error("Failed to load sky panorama texture: textures/sky_panorama.png");

            VkDescriptorImageInfo skyInfo{};
            skyInfo.sampler = skyTexture->sampler;
            skyInfo.imageView = skyTexture->view;
            skyInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet skyWrite{};
            skyWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            skyWrite.dstSet = m_skySet;
            skyWrite.dstBinding = 0;
            skyWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            skyWrite.descriptorCount = 1;
            skyWrite.pImageInfo = &skyInfo;
            vkUpdateDescriptorSets(device, 1, &skyWrite, 0, nullptr);

            std::string variant = shaderVariant("default");
            createFullscreenBundlePipeline(
                device,
                m_offscreenRenderPass,
                {m_frameOnlyLayout, m_skySetLayout},
                shaderBundlePath("Sky"),
                variant,
                false,
                false,
                false,
                m_skyPipeline,
                m_skyPipelineLayout,
                "sky");

            const bool gridUsesDepth = true;
            createFullscreenBundlePipeline(
                device,
                m_offscreenRenderPass,
                {m_frameOnlyLayout},
                shaderBundlePath("Grid"),
                variant,
                true,
                gridUsesDepth,
                false,
                m_gridPipeline,
                m_gridPipelineLayout,
                "grid");
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
        m_assetManager.setPerFrameLayout(m_perFrameLayout);
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

    // 初始化后处理管线
    m_postProcess.init(m_context, m_layoutCache, m_offscreenWidth, m_offscreenHeight);

    // --- 6. 初始化 IBL 生成器并生成 IBL 贴图 ---
    if (!m_iblGenerator.isGenerated()) {
        m_iblGenerator.init(m_context, m_layoutCache, m_commandManager);
        m_iblGenerator.generateBrdfLut();

        // 使用天空盒全景纹理生成 IBL cubemap + irradiance + prefiltered
        const TextureAsset* skyTex = m_assetManager.loadTexture("textures/sky_panorama.png");
        if (skyTex && skyTex->view != VK_NULL_HANDLE) {
            m_iblGenerator.generate(skyTex->view, skyTex->sampler);
            // 更新 per-frame descriptor set 的 IBL binding
            writeIBLDescriptors();
        }
    }
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

    // 3. Rebuild pipelines from ShaderBundle
    {
        std::string triPath = std::string(ASSETS_DIR) + "/shaders/Triangle.shaderbundle";
        ShaderBundle triBundle;
        std::string var = shaderVariant("default");
        if (!triBundle.load(triPath) || !triBundle.hasVariant(var))
            throw std::runtime_error("Failed to load Triangle.shaderbundle for reload: " + var);

        auto vertSpv = triBundle.getVertSpv(var);
        auto fragSpv = triBundle.getFragSpv(var);
        auto reflectJson = triBundle.getReflectJson(var);

        m_offscreenPipeline.cleanup(device);
        m_wireframePipeline.cleanup(device);
        m_offscreenPipeline.createFromMemory(device, m_offscreenRenderPass,
            {m_offscreenWidth, m_offscreenHeight}, m_layoutCache, VK_POLYGON_MODE_FILL,
            vertSpv, fragSpv, reflectJson, m_perFrameLayout);
        m_wireframePipeline.createFromMemory(device, m_offscreenRenderPass,
            {m_offscreenWidth, m_offscreenHeight}, m_layoutCache, VK_POLYGON_MODE_LINE,
            vertSpv, fragSpv, reflectJson, m_perFrameLayout);

        // 3b. Rebuild bindless pipelines
        std::string bVar = shaderVariant("bindless");
        if (m_bindlessEnabled && m_bindlessSetLayout != VK_NULL_HANDLE
            && triBundle.hasVariant(bVar)) {
            m_bindlessOffscreenPipeline.cleanup(device);
            m_bindlessWireframePipeline.cleanup(device);

            std::vector<VkDescriptorSetLayout> bindlessLayouts = {
                m_perFrameLayout,
                m_bindlessSetLayout
            };

            auto bVertSpv = triBundle.getVertSpv(bVar);
            auto bFragSpv = triBundle.getFragSpv(bVar);
            auto bReflectJson = triBundle.getReflectJson(bVar);

            ShaderReflectionData bindlessReflection;
            bindlessReflection.loadFromString(bReflectJson);
            auto pcRanges = bindlessReflection.createPushConstantRanges();

            m_bindlessOffscreenPipeline.createWithLayoutsFromMemory(
                device, m_offscreenRenderPass, bindlessLayouts,
                {m_offscreenWidth, m_offscreenHeight}, pcRanges, VK_POLYGON_MODE_FILL,
                bVertSpv, bFragSpv, bReflectJson);

            m_bindlessWireframePipeline.createWithLayoutsFromMemory(
                device, m_offscreenRenderPass, bindlessLayouts,
                {m_offscreenWidth, m_offscreenHeight}, pcRanges, VK_POLYGON_MODE_LINE,
                bVertSpv, bFragSpv, bReflectJson);
        }

        // 4. Rebuild main pipeline
        m_pipeline.cleanup(device);
        m_pipeline.createFromMemory(device, m_renderPass.get(), m_swapChain.getExtent(),
            m_layoutCache, VK_POLYGON_MODE_FILL, vertSpv, fragSpv, reflectJson, m_perFrameLayout);

        // 5. Rebuild sky and grid fullscreen pipelines
        createFullscreenBundlePipeline(
            device,
            m_offscreenRenderPass,
            {m_frameOnlyLayout, m_skySetLayout},
            shaderBundlePath("Sky"),
            var,
            false,
            false,
            false,
            m_skyPipeline,
            m_skyPipelineLayout,
            "sky");

        const bool gridUsesDepth = true;
        createFullscreenBundlePipeline(
            device,
            m_offscreenRenderPass,
            {m_frameOnlyLayout},
            shaderBundlePath("Grid"),
            var,
            true,
            gridUsesDepth,
            false,
            m_gridPipeline,
            m_gridPipelineLayout,
            "grid");
    }

    // 重新加载后处理着色器
    m_postProcess.reloadShaders();
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

    // 重新创建后处理资源
    m_postProcess.resize(width, height);
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

    // --- Draw sky and grid (before scene objects) ---
    if (m_skyPipeline != VK_NULL_HANDLE && m_skySet != VK_NULL_HANDLE) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
        VkDescriptorSet frameOnlySet = m_frameOnlySets[m_currentFrame];
        VkDescriptorSet skySets[] = {frameOnlySet, m_skySet};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_skyPipelineLayout, 0, 2, skySets, 0, nullptr);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    // Build frustum for CPU-side culling
    Frustum frustum;
    if (m_camera) {
        float aspect = m_offscreenWidth / static_cast<float>(m_offscreenHeight);
        bool vulkanYFlip = !vkIsDirectXBackend() && !vkIsOpenGLBackend() && !vkIsGLESBackend();
        glm::mat4 proj = m_camera->getProjMatrix(aspect, vulkanYFlip);
        if (vkIsGLESBackend() && !vkHasClipControl()) {
            glm::mat4 depthFix(1.0f); depthFix[2][2] = 2.0f; depthFix[3][2] = -1.0f;
            proj = depthFix * proj;
        }
        glm::mat4 vp = proj * m_camera->getViewMatrix();
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
            if (node->isLight()) return;

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
            pc.model = toShaderMatrix(node->getWorldMatrix());
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
            if (node->isLight()) return;

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
            pc.model = toShaderMatrix(node->getWorldMatrix());
            pc.highlighted = 0;
            vkCmdPushConstants(commandBuffer, pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0, sizeof(PushConstantData), &pc);

            if (vkIsGLESBackend() || vkIsOpenGLBackend()) {
                static int s_glClipLog = 0;
                if (s_glClipLog < 8 && (node->name == "Ground" || node->name == "Center Cube" || node->name == "Sphere")) {
                    glm::mat4 view = m_camera ? m_camera->getViewMatrix()
                                              : glm::lookAt(glm::vec3(2,2,2), glm::vec3(0,0,0), glm::vec3(0,1,0));
                    bool needYFlip = !vkIsDirectXBackend() && !vkIsOpenGLBackend() && !vkIsGLESBackend();
                    glm::mat4 proj = m_camera ? m_camera->getProjMatrix(m_offscreenWidth / static_cast<float>(m_offscreenHeight), needYFlip)
                                              : glm::perspective(glm::radians(45.0f), m_offscreenWidth / static_cast<float>(m_offscreenHeight), 0.1f, 10.0f);
                    if (vkIsGLESBackend() && !vkHasClipControl()) {
                        glm::mat4 depthFix(1.0f);
                        depthFix[2][2] = 2.0f;
                        depthFix[3][2] = -1.0f;
                        proj = depthFix * proj;
                    }
                    glm::vec4 worldCenter = node->getWorldMatrix() * glm::vec4(0, 0, 0, 1);
                    glm::vec4 clip = proj * view * worldCenter;
                    glm::vec3 ndc = glm::vec3(clip) / clip.w;
                    SDL_Log("[Renderer][%s] node=%s clip=(%.3f, %.3f, %.3f, %.3f) ndc=(%.3f, %.3f, %.3f)",
                            vkIsGLESBackend() ? "GLES" : "OpenGL",
                            node->name.c_str(), clip.x, clip.y, clip.z, clip.w, ndc.x, ndc.y, ndc.z);
                    s_glClipLog++;
                }
            }

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

    if (m_gridPipeline != VK_NULL_HANDLE) {
        VkDescriptorSet frameOnlySet = m_frameOnlySets[m_currentFrame];
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_gridPipelineLayout, 0, 1, &frameOnlySet, 0, nullptr);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
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
        pc.model = toShaderMatrix(selected->getWorldMatrix());
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
        pc.model = toShaderMatrix(glm::scale(glm::translate(glm::mat4(1.0f), pos), glm::vec3(0.15f)));
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

        pc.model = toShaderMatrix(glm::translate(glm::mat4(1.0f), arrowCenter)
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
        glm::mat4 view = m_camera->getViewMatrix();
        bool needYFlip = !vkIsDirectXBackend() && !vkIsOpenGLBackend() && !vkIsGLESBackend();
        glm::mat4 proj = m_camera->getProjMatrix(aspect, needYFlip);
        // GLES 没有 glClipControl: GLM_FORCE_DEPTH_ZERO_TO_ONE 输出 [0,1]
        // 但 GLES NDC 深度是 [-1,1]，需要修正: z_ndc = 2*z_01 - 1
        if (vkIsGLESBackend() && !vkHasClipControl()) {
            // GLES 没有 glClipControl，NDC 深度 [-1,1]
            // GLM_FORCE_DEPTH_ZERO_TO_ONE 输出 [0,1]，需要修正: z_new = 2*z - w
            glm::mat4 depthFix(1.0f);
            depthFix[2][2] = 2.0f;
            depthFix[3][2] = -1.0f;
            proj = depthFix * proj;
        }
        ubo.view = glm::transpose(view);
        ubo.proj = glm::transpose(proj);
    } else {
        ubo.view = glm::lookAt(glm::vec3(2,2,2), glm::vec3(0,0,0), glm::vec3(0,1,0));
        ubo.proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
        if (!vkIsDirectXBackend() && !vkIsOpenGLBackend() && !vkIsGLESBackend()) ubo.proj[1][1] *= -1;
        if (vkIsGLESBackend() && !vkHasClipControl()) {
            glm::mat4 depthFix(1.0f);
            depthFix[2][2] = 2.0f;
            depthFix[3][2] = -1.0f;
            ubo.proj = depthFix * ubo.proj;
        }
        ubo.view = glm::transpose(ubo.view);
        ubo.proj = glm::transpose(ubo.proj);
    }

    ubo.ambientColor = glm::vec3(0.15f, 0.15f, 0.15f);
    if (m_camera)
        ubo.cameraPos = m_camera->getPosition();
    else
        ubo.cameraPos = glm::vec3(2.0f, 2.0f, 2.0f);

    // Collect all lights into UBO array
    int lightCount = 0;
    if (m_activeScene) {
        m_activeScene->traverseNodes([&](Node* node) {
            if (lightCount >= MAX_LIGHTS) return;
            if (!node->isLight()) return;

            LightData& ld = ubo.lights[lightCount];
            glm::vec3 worldPos = glm::vec3(node->getWorldMatrix()[3]);
            glm::vec3 dir = node->getLightDirection();
            glm::vec3 color = node->lightColor * node->lightIntensity;

            if (node->nodeType == NodeType::DirectionalLight) {
                ld.positionAndType = glm::vec4(0.0f, 0.0f, 0.0f, float(LIGHT_TYPE_DIRECTIONAL));
                ld.directionAndRange = glm::vec4(dir, 0.0f);
            } else if (node->nodeType == NodeType::PointLight) {
                ld.positionAndType = glm::vec4(worldPos, float(LIGHT_TYPE_POINT));
                ld.directionAndRange = glm::vec4(0.0f, 0.0f, 0.0f, node->lightRange);
            } else if (node->nodeType == NodeType::SpotLight) {
                ld.positionAndType = glm::vec4(worldPos, float(LIGHT_TYPE_SPOT));
                ld.directionAndRange = glm::vec4(dir, node->lightRange);
                ld.spotParams = glm::vec4(
                    glm::cos(glm::radians(node->spotInnerAngle)),
                    glm::cos(glm::radians(node->spotOuterAngle)),
                    0.0f, 0.0f);
            }
            ld.colorAndIntensity = glm::vec4(color, node->lightIntensity);
            lightCount++;
        });
    }
    // Fallback: if no lights in scene, add a default directional light
    if (lightCount == 0) {
        LightData& ld = ubo.lights[0];
        ld.positionAndType = glm::vec4(0, 0, 0, float(LIGHT_TYPE_DIRECTIONAL));
        ld.directionAndRange = glm::vec4(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 0.0f);
        ld.colorAndIntensity = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        ld.spotParams = glm::vec4(0.0f);
        lightCount = 1;
    }
    ubo.lightCountPad = glm::ivec4(lightCount, 0, 0, 0);

    // Compute shadow map light VP (first directional light)
    bool hasShadow = false;
    for (int i = 0; i < lightCount; i++) {
        if (int(ubo.lights[i].positionAndType.w) == LIGHT_TYPE_DIRECTIONAL) {
            glm::vec3 lightDir = glm::vec3(ubo.lights[i].directionAndRange);
            // Orthographic projection centered on origin, covering scene bounds
            glm::vec3 lightPos = -lightDir * 20.0f; // 光源位置 = 反方向 * 距离
            glm::mat4 lightView = glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0, 1, 0));
            glm::mat4 lightProj = glm::ortho(-15.0f, 15.0f, -15.0f, 15.0f, 0.1f, 50.0f);
            if (!vkIsDirectXBackend() && !vkIsOpenGLBackend() && !vkIsGLESBackend()) lightProj[1][1] *= -1; // Vulkan Y-flip
            if (vkIsGLESBackend() && !vkHasClipControl()) {
                // GLES 没有 glClipControl，NDC 深度 [-1,1]，需要修正 lightProj
                glm::mat4 depthFix(1.0f);
                depthFix[2][2] = 2.0f;
                depthFix[3][2] = -1.0f;
                lightProj = depthFix * lightProj;
            }
            glm::mat4 lightVP = lightProj * lightView;
            ubo.lightVP = glm::transpose(lightVP);
            hasShadow = true;
            break;
        }
    }
    // x=shadowEnabled, y=shadowUVFlipY, z=depthNDCConvert (GLES 需要 [-1,1]→[0,1] 转换)
    // D3D: viewport Y-up 导致 shadow map UV 需要翻转
    // OpenGL + glClipControl(GL_LOWER_LEFT): 和 Vulkan 一致，不需要翻转
    // 目前 Android GLES 上主方向光会被 shadow 分支整体乘暗，先关闭 GLES 阴影采样，
    // 确保直射光链路恢复正常，再继续定位 shadowMap/sampleShadow 的根因。
    const bool enableShadowSampling = hasShadow;
    ubo.shadowParams = glm::ivec4(enableShadowSampling ? 1 : 0, vkIsDirectXBackend() ? 1 : 0,
                                  (vkIsGLESBackend() && !vkHasClipControl()) ? 1 : 0, 0);

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

// ========================================================================
// Shadow Map
// ========================================================================

void Renderer::createShadowResources()
{
    VkDevice device = m_context.getDevice();
    constexpr uint32_t size = SHADOW_MAP_SIZE;
    const bool glesShadowColor = vkIsGLESBackend();
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkFormat shadowSampleFormat = glesShadowColor ? VK_FORMAT_R32_SFLOAT : depthFormat;

    // Shadow sample image: GLES 用颜色图存深度，其他后端仍用深度纹理。
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = shadowSampleFormat;
    imageInfo.extent = {size, size, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = (glesShadowColor ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                       : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
                    | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device, &imageInfo, nullptr, &m_shadowImage);

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, m_shadowImage, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = m_context.findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &m_shadowMemory);
    vkBindImageMemory(device, m_shadowImage, m_shadowMemory, 0);

    // Image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_shadowImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = shadowSampleFormat;
    viewInfo.subresourceRange.aspectMask = glesShadowColor ? VK_IMAGE_ASPECT_COLOR_BIT
                                                           : VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(device, &viewInfo, nullptr, &m_shadowImageView);

    if (glesShadowColor) {
        VkImageCreateInfo depthInfo = imageInfo;
        depthInfo.format = depthFormat;
        depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        vkCreateImage(device, &depthInfo, nullptr, &m_shadowDepthImage);

        vkGetImageMemoryRequirements(device, m_shadowDepthImage, &memReq);
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = m_context.findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &m_shadowDepthMemory);
        vkBindImageMemory(device, m_shadowDepthImage, m_shadowDepthMemory, 0);

        VkImageViewCreateInfo depthViewInfo{};
        depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depthViewInfo.image = m_shadowDepthImage;
        depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format = depthFormat;
        depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthViewInfo.subresourceRange.levelCount = 1;
        depthViewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(device, &depthViewInfo, nullptr, &m_shadowDepthImageView);
    }

    // Sampler (linear filtering for PCF, border color = white = max depth)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    vkCreateSampler(device, &samplerInfo, nullptr, &m_shadowSampler);

    VkAttachmentDescription colorAttach{};
    colorAttach.format = shadowSampleFormat;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttach{};
    depthAttach.format = depthFormat;
    depthAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp = glesShadowColor ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttach.finalLayout = glesShadowColor
        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = glesShadowColor ? 1 : 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (glesShadowColor) {
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
    }
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    std::array<VkAttachmentDescription, 2> shadowAttachments = {colorAttach, depthAttach};
    rpInfo.attachmentCount = glesShadowColor ? 2u : 1u;
    rpInfo.pAttachments = glesShadowColor ? shadowAttachments.data() : &depthAttach;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dep;
    vkCreateRenderPass(device, &rpInfo, nullptr, &m_shadowRenderPass);

    // Framebuffer
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_shadowRenderPass;
    std::array<VkImageView, 2> shadowViews = {m_shadowImageView, m_shadowDepthImageView};
    fbInfo.attachmentCount = glesShadowColor ? 2u : 1u;
    fbInfo.pAttachments = glesShadowColor ? shadowViews.data() : &m_shadowImageView;
    fbInfo.width = size;
    fbInfo.height = size;
    fbInfo.layers = 1;
    vkCreateFramebuffer(device, &fbInfo, nullptr, &m_shadowFramebuffer);

    // Shadow pipeline
    {
        std::vector<char> vertSpv;
        std::vector<char> fragSpv;
        if (glesShadowColor) {
            static const char* kGlesShadowVS = R"(#version 300 es
precision highp float;
layout(std140) uniform block_FrameData_std140_0 {
    mat4 view_0;
    mat4 proj_0;
    vec3 ambientColor_0; float _framePad0;
    vec3 cameraPos_0; float _framePad1;
    ivec4 lightCountPad_0;
    vec4 lights_0[32];
    mat4 lightVP_0;
    ivec4 shadowParams_0;
} frame_0;
layout(std140) uniform block_PushConstants_std140_0 {
    mat4 model_0;
    int highlighted_0;
    vec3 _pcPad0;
} pc_0;
layout(location = 0) in vec3 input_position_0;
void main() {
    vec4 worldPos = pc_0.model_0 * vec4(input_position_0, 1.0);
    gl_Position = transpose(frame_0.lightVP_0) * worldPos;
})";
            static const char* kGlesShadowFS = R"(#version 300 es
precision highp float;
layout(location = 0) out float outShadow_0;
void main() {
    outShadow_0 = gl_FragCoord.z;
})";
            vertSpv.assign(kGlesShadowVS, kGlesShadowVS + std::strlen(kGlesShadowVS));
            fragSpv.assign(kGlesShadowFS, kGlesShadowFS + std::strlen(kGlesShadowFS));
        } else {
            std::string bundlePath = std::string(ASSETS_DIR) + "/shaders/Shadow.shaderbundle";
            ShaderBundle bundle;
            std::string sVar = shaderVariant("default");
            if (!bundle.load(bundlePath) || !bundle.hasVariant(sVar))
                throw std::runtime_error("Failed to load Shadow.shaderbundle variant: " + sVar);
            vertSpv = bundle.getVertSpv(sVar);
            fragSpv = bundle.getFragSpv(sVar);
        }

        auto createModule = [&](const std::vector<char>& code) -> VkShaderModule {
            VkShaderModuleCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            ci.codeSize = code.size();
            ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
            VkShaderModule mod;
            vkCreateShaderModule(device, &ci, nullptr, &mod);
            return mod;
        };

        VkShaderModule vertMod = createModule(vertSpv);
        VkShaderModule fragMod = createModule(fragSpv);

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName = "main";

        auto bindingDesc = Vertex::getBindingDescription();
        auto attrDescs = Vertex::getAttributeDescriptions();
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &bindingDesc;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
        vertexInput.pVertexAttributeDescriptions = attrDescs.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineDynamicStateCreateInfo dynamicState{};
        std::vector<VkDynamicState> dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
        dynamicState.pDynamicStates = dynStates.data();

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; // Front-face culling reduces shadow acne
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_TRUE;
        rasterizer.depthBiasConstantFactor = 1.25f;
        rasterizer.depthBiasSlopeFactor = 1.75f;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState shadowColorBlend{};
        shadowColorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        shadowColorBlend.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = glesShadowColor ? 1u : 0u;
        colorBlending.pAttachments = glesShadowColor ? &shadowColorBlend : nullptr;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        // Pipeline layout: set 0 only + push constants
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(PushConstantData);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_perFrameLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pcRange;
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_shadowPipelineLayout);

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
        pipelineInfo.layout = m_shadowPipelineLayout;
        pipelineInfo.renderPass = m_shadowRenderPass;

        VkResult shadowPipeResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_shadowVkPipeline);
        if (shadowPipeResult != VK_SUCCESS)
            m_shadowVkPipeline = VK_NULL_HANDLE;

        vkDestroyShaderModule(device, fragMod, nullptr);
        vkDestroyShaderModule(device, vertMod, nullptr);
    }
}

void Renderer::destroyShadowResources()
{
    VkDevice device = m_context.getDevice();
    if (m_shadowVkPipeline) vkDestroyPipeline(device, m_shadowVkPipeline, nullptr);
    if (m_shadowPipelineLayout) vkDestroyPipelineLayout(device, m_shadowPipelineLayout, nullptr);
    if (m_shadowFramebuffer) vkDestroyFramebuffer(device, m_shadowFramebuffer, nullptr);
    if (m_shadowRenderPass) vkDestroyRenderPass(device, m_shadowRenderPass, nullptr);
    if (m_shadowSampler) vkDestroySampler(device, m_shadowSampler, nullptr);
    if (m_shadowImageView) vkDestroyImageView(device, m_shadowImageView, nullptr);
    if (m_shadowDepthImageView) vkDestroyImageView(device, m_shadowDepthImageView, nullptr);
    if (m_shadowImage) vkDestroyImage(device, m_shadowImage, nullptr);
    if (m_shadowDepthImage) vkDestroyImage(device, m_shadowDepthImage, nullptr);
    if (m_shadowMemory) vkFreeMemory(device, m_shadowMemory, nullptr);
    if (m_shadowDepthMemory) vkFreeMemory(device, m_shadowDepthMemory, nullptr);
}

void Renderer::renderShadowPass(VkCommandBuffer cmd, Scene& scene)
{
    if (m_shadowVkPipeline == VK_NULL_HANDLE) return;

    constexpr uint32_t size = SHADOW_MAP_SIZE;

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = m_shadowRenderPass;
    rpBegin.framebuffer = m_shadowFramebuffer;
    rpBegin.renderArea = {{0, 0}, {size, size}};
    std::array<VkClearValue, 2> clearValues{};
    if (vkIsGLESBackend()) {
        clearValues[0].color = {{1.0f, 1.0f, 1.0f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};
        rpBegin.clearValueCount = 2;
    } else {
        clearValues[0].depthStencil = {1.0f, 0};
        rpBegin.clearValueCount = 1;
    }
    rpBegin.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowVkPipeline);

    VkViewport vp{0, 0, float(size), float(size), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {size, size}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind per-frame descriptor set (contains lightVP in view/proj slots via UBO)
    VkDescriptorSet frameSet = m_descriptor.getPerFrameSet(m_currentFrame);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_shadowPipelineLayout, 0, 1, &frameSet, 0, nullptr);

    // Render all mesh nodes (depth only)
    scene.traverseNodes([&](Node* node) {
        if (node->isLight()) return;
        if (node->meshType == MeshType::None && node->meshPath.empty()) return;

        PushConstantData pc{};
        pc.model = toShaderMatrix(node->getWorldMatrix());
        pc.highlighted = 0;
        vkCmdPushConstants(cmd, m_shadowPipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(PushConstantData), &pc);

        if (!node->meshPath.empty()) {
            auto* mesh = m_assetManager.loadMesh(node->meshPath);
            if (mesh) {
                VkBuffer buffers[] = {mesh->vertexBuffer};
                VkDeviceSize offsets[] = {0};
                vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
                vkCmdBindIndexBuffer(cmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
            }
        } else if (node->meshType != MeshType::None) {
            m_meshLibrary.bind(cmd, node->meshType);
            vkCmdDrawIndexed(cmd, m_meshLibrary.getIndexCount(node->meshType), 1, 0, 0, 0);
        }
    });

    vkCmdEndRenderPass(cmd);
}

} // namespace QymEngine
