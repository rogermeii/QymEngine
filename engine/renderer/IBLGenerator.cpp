#include "renderer/IBLGenerator.h"
#include "renderer/VulkanContext.h"
#include "renderer/DescriptorLayoutCache.h"
#include "renderer/CommandManager.h"
#include "renderer/VkDispatch.h"
#include "asset/ShaderBundle.h"

#include <SDL.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <string>
#include <iostream>

namespace QymEngine {

// ========================================================================
// Push constant 结构体 (与 IBL 着色器一一对应)
// ========================================================================

struct IBLPushConstant {
    int32_t faceIndex;
    float   roughness;
};
static_assert(sizeof(IBLPushConstant) == 8, "IBLPushConstant size mismatch");

// ========================================================================
// 着色器变体/路径辅助函数
// ========================================================================

static std::string iblShaderVariant(const std::string& base) {
    if (vkIsD3D12Backend())
        return base + "_dxil";
    if (vkIsD3D11Backend())
        return base + "_dxbc";
    if (vkIsOpenGLBackend() || vkIsGLESBackend())
        return base + "_glsl";
    return base;
}

static std::string iblShaderBundlePath(const char* bundleBaseName) {
#ifdef __ANDROID__
    return std::string("shaders/ibl/") + bundleBaseName + ".shaderbundle";
#else
    return std::string(ASSETS_DIR) + "/shaders/ibl/" + bundleBaseName + ".shaderbundle";
#endif
}

// ========================================================================
// createFullscreenBundlePipeline (与 PostProcess.cpp 同构)
// ========================================================================

static void createFullscreenBundlePipeline(
    VkDevice device,
    VkRenderPass renderPass,
    const std::vector<VkDescriptorSetLayout>& setLayouts,
    const std::string& bundlePath,
    const std::string& variant,
    VkPipeline& outPipeline,
    VkPipelineLayout& outLayout,
    const char* debugName,
    const VkPushConstantRange* pushConstantRange = nullptr)
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
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

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

// ========================================================================
// 辅助: 创建 color-only render pass
// ========================================================================

static VkRenderPass createColorOnlyRenderPass(VkDevice device, VkFormat format) {
    VkAttachmentDescription colorAttach{};
    colorAttach.format = format;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAttach;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dep;

    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device, &rpInfo, nullptr, &rp) != VK_SUCCESS)
        throw std::runtime_error("failed to create IBL render pass!");
    return rp;
}

// ========================================================================
// init / destroy
// ========================================================================

void IBLGenerator::init(VulkanContext& ctx, DescriptorLayoutCache& layoutCache, CommandManager& cmdMgr)
{
    m_context = &ctx;
    m_layoutCache = &layoutCache;
    m_cmdMgr = &cmdMgr;

    createRenderPasses();
    createResources();
    createPipelines();
}

void IBLGenerator::destroy()
{
    if (!m_context) return;
    VkDevice device = m_context->getDevice();

    destroyPipelines();
    destroyResources();
    destroyRenderPasses();

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    m_samplerSetLayout = VK_NULL_HANDLE; // 由 DescriptorLayoutCache 管理
    m_context = nullptr;
    m_generated = false;
}

// ========================================================================
// createResources
// ========================================================================

void IBLGenerator::createCubemapImage(VkImage& image, VkDeviceMemory& memory,
                                       uint32_t size, uint32_t mipLevels, VkFormat format)
{
    VkDevice device = m_context->getDevice();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {size, size, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 6;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS)
        throw std::runtime_error("failed to create IBL cubemap image!");

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, image, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = m_context->findMemoryType(memReqs.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate IBL cubemap image memory!");

    vkBindImageMemory(device, image, memory, 0);
}

VkImageView IBLGenerator::createCubeFaceView(VkImage image, VkFormat format,
                                              uint32_t face, uint32_t mip)
{
    VkDevice device = m_context->getDevice();

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = mip;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = face;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS)
        throw std::runtime_error("failed to create IBL cubemap face view!");
    return view;
}

VkImageView IBLGenerator::createCubeView(VkImage image, VkFormat format, uint32_t mipLevels)
{
    VkDevice device = m_context->getDevice();

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS)
        throw std::runtime_error("failed to create IBL cube view!");
    return view;
}

VkFramebuffer IBLGenerator::createFramebuffer(VkRenderPass rp, VkImageView faceView,
                                               uint32_t width, uint32_t height)
{
    VkDevice device = m_context->getDevice();

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = rp;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &faceView;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;

    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &fb) != VK_SUCCESS)
        throw std::runtime_error("failed to create IBL framebuffer!");
    return fb;
}

void IBLGenerator::transitionImageLayout(VkCommandBuffer cmd, VkImage image,
                                          VkImageLayout oldLayout, VkImageLayout newLayout,
                                          uint32_t mipLevels, uint32_t layerCount)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layerCount;

    VkPipelineStageFlags srcStage, dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
                          0, nullptr, 0, nullptr, 1, &barrier);
}

void IBLGenerator::createResources()
{
    VkDevice device = m_context->getDevice();
    VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat brdfFormat = VK_FORMAT_R16G16_SFLOAT;

    // --- 创建 cubemap images ---
    createCubemapImage(m_cubemapImage, m_cubemapMemory, CUBEMAP_SIZE, 1, hdrFormat);
    createCubemapImage(m_irradianceImage, m_irradianceMemory, IRRADIANCE_SIZE, 1, hdrFormat);
    createCubemapImage(m_prefilteredImage, m_prefilteredMemory, PREFILTER_SIZE, PREFILTER_MIP_LEVELS, hdrFormat);

    // --- 创建 face views (用于 framebuffer attachment) ---
    for (uint32_t f = 0; f < 6; f++) {
        m_cubemapFaceViews[f] = createCubeFaceView(m_cubemapImage, hdrFormat, f, 0);
        m_irradianceFaceViews[f] = createCubeFaceView(m_irradianceImage, hdrFormat, f, 0);
    }
    for (uint32_t mip = 0; mip < PREFILTER_MIP_LEVELS; mip++) {
        for (uint32_t f = 0; f < 6; f++) {
            m_prefilteredFaceViews[mip * 6 + f] = createCubeFaceView(m_prefilteredImage, hdrFormat, f, mip);
        }
    }

    // --- 创建 cube views (用于着色器采样) ---
    m_cubemapCubeView = createCubeView(m_cubemapImage, hdrFormat, 1);
    m_irradianceCubeView = createCubeView(m_irradianceImage, hdrFormat, 1);
    m_prefilteredCubeView = createCubeView(m_prefilteredImage, hdrFormat, PREFILTER_MIP_LEVELS);

    // --- 创建 BRDF LUT image ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {BRDF_LUT_SIZE, BRDF_LUT_SIZE, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = brdfFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &imageInfo, nullptr, &m_brdfLutImage) != VK_SUCCESS)
            throw std::runtime_error("failed to create BRDF LUT image!");

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, m_brdfLutImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = m_context->findMemoryType(memReqs.memoryTypeBits,
                                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &allocInfo, nullptr, &m_brdfLutMemory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate BRDF LUT image memory!");

        vkBindImageMemory(device, m_brdfLutImage, m_brdfLutMemory, 0);

        // BRDF LUT view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_brdfLutImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = brdfFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &m_brdfLutView) != VK_SUCCESS)
            throw std::runtime_error("failed to create BRDF LUT image view!");
    }

    // --- 创建 sampler ---
    {
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
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(PREFILTER_MIP_LEVELS);

        if (vkCreateSampler(device, &samplerInfo, nullptr, &m_cubemapSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL cubemap sampler!");

        // BRDF LUT 用独立 sampler（不需要 mipmap）
        VkSamplerCreateInfo brdfSamplerInfo = samplerInfo;
        brdfSamplerInfo.maxLod = 0.0f;

        if (vkCreateSampler(device, &brdfSamplerInfo, nullptr, &m_brdfLutSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL BRDF LUT sampler!");
    }

    // --- 创建 descriptor pool & sets ---
    {
        // 3 个描述符集: equirect, irradiance, prefilter
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 3;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 3;

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL descriptor pool!");

        // 获取/创建 sampler layout (1 个 combined image sampler)
        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding = 0;
        samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        m_samplerSetLayout = m_layoutCache->getOrCreate(device, {samplerBinding});

        // 分配描述符集
        auto allocSet = [&]() -> VkDescriptorSet {
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = m_descriptorPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &m_samplerSetLayout;

            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(device, &allocInfo, &set) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate IBL descriptor set!");
            return set;
        };

        m_equirectSet = allocSet();
        m_irradianceSet = allocSet();
        m_prefilterSet = allocSet();
    }
}

void IBLGenerator::destroyResources()
{
    VkDevice device = m_context->getDevice();

    // Samplers
    if (m_cubemapSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_cubemapSampler, nullptr);
        m_cubemapSampler = VK_NULL_HANDLE;
    }
    if (m_brdfLutSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_brdfLutSampler, nullptr);
        m_brdfLutSampler = VK_NULL_HANDLE;
    }

    // Cubemap
    for (auto& v : m_cubemapFaceViews) { if (v) { vkDestroyImageView(device, v, nullptr); v = VK_NULL_HANDLE; } }
    if (m_cubemapCubeView) { vkDestroyImageView(device, m_cubemapCubeView, nullptr); m_cubemapCubeView = VK_NULL_HANDLE; }
    if (m_cubemapImage) { vkDestroyImage(device, m_cubemapImage, nullptr); m_cubemapImage = VK_NULL_HANDLE; }
    if (m_cubemapMemory) { vkFreeMemory(device, m_cubemapMemory, nullptr); m_cubemapMemory = VK_NULL_HANDLE; }

    // Irradiance
    for (auto& v : m_irradianceFaceViews) { if (v) { vkDestroyImageView(device, v, nullptr); v = VK_NULL_HANDLE; } }
    if (m_irradianceCubeView) { vkDestroyImageView(device, m_irradianceCubeView, nullptr); m_irradianceCubeView = VK_NULL_HANDLE; }
    if (m_irradianceImage) { vkDestroyImage(device, m_irradianceImage, nullptr); m_irradianceImage = VK_NULL_HANDLE; }
    if (m_irradianceMemory) { vkFreeMemory(device, m_irradianceMemory, nullptr); m_irradianceMemory = VK_NULL_HANDLE; }

    // Prefiltered
    for (auto& v : m_prefilteredFaceViews) { if (v) { vkDestroyImageView(device, v, nullptr); v = VK_NULL_HANDLE; } }
    if (m_prefilteredCubeView) { vkDestroyImageView(device, m_prefilteredCubeView, nullptr); m_prefilteredCubeView = VK_NULL_HANDLE; }
    if (m_prefilteredImage) { vkDestroyImage(device, m_prefilteredImage, nullptr); m_prefilteredImage = VK_NULL_HANDLE; }
    if (m_prefilteredMemory) { vkFreeMemory(device, m_prefilteredMemory, nullptr); m_prefilteredMemory = VK_NULL_HANDLE; }

    // BRDF LUT
    if (m_brdfLutView) { vkDestroyImageView(device, m_brdfLutView, nullptr); m_brdfLutView = VK_NULL_HANDLE; }
    if (m_brdfLutImage) { vkDestroyImage(device, m_brdfLutImage, nullptr); m_brdfLutImage = VK_NULL_HANDLE; }
    if (m_brdfLutMemory) { vkFreeMemory(device, m_brdfLutMemory, nullptr); m_brdfLutMemory = VK_NULL_HANDLE; }
}

// ========================================================================
// createRenderPasses / destroyRenderPasses
// ========================================================================

void IBLGenerator::createRenderPasses()
{
    VkDevice device = m_context->getDevice();
    m_hdrRenderPass = createColorOnlyRenderPass(device, VK_FORMAT_R16G16B16A16_SFLOAT);
    m_brdfRenderPass = createColorOnlyRenderPass(device, VK_FORMAT_R16G16_SFLOAT);
}

void IBLGenerator::destroyRenderPasses()
{
    VkDevice device = m_context->getDevice();
    if (m_hdrRenderPass) { vkDestroyRenderPass(device, m_hdrRenderPass, nullptr); m_hdrRenderPass = VK_NULL_HANDLE; }
    if (m_brdfRenderPass) { vkDestroyRenderPass(device, m_brdfRenderPass, nullptr); m_brdfRenderPass = VK_NULL_HANDLE; }
}

// ========================================================================
// createPipelines / destroyPipelines
// ========================================================================

void IBLGenerator::createPipelines()
{
    VkDevice device = m_context->getDevice();
    std::string variant = iblShaderVariant("default");

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(IBLPushConstant);

    // EquirectToCubemap: 需要输入纹理
    createFullscreenBundlePipeline(device, m_hdrRenderPass,
        {m_samplerSetLayout}, iblShaderBundlePath("EquirectToCubemap"), variant,
        m_equirectPipeline, m_equirectPipelineLayout, "IBL equirect", &pcRange);

    // IrradianceConvolution: 需要 cubemap 采样
    createFullscreenBundlePipeline(device, m_hdrRenderPass,
        {m_samplerSetLayout}, iblShaderBundlePath("IrradianceConvolution"), variant,
        m_irradiancePipeline, m_irradiancePipelineLayout, "IBL irradiance", &pcRange);

    // PrefilterEnvMap: 需要 cubemap 采样 + roughness
    createFullscreenBundlePipeline(device, m_hdrRenderPass,
        {m_samplerSetLayout}, iblShaderBundlePath("PrefilterEnvMap"), variant,
        m_prefilterPipeline, m_prefilterPipelineLayout, "IBL prefilter", &pcRange);

    // BrdfLut: 无输入纹理，无描述符集
    createFullscreenBundlePipeline(device, m_brdfRenderPass,
        {}, iblShaderBundlePath("BrdfLut"), variant,
        m_brdfPipeline, m_brdfPipelineLayout, "IBL BRDF LUT");
}

void IBLGenerator::destroyPipelines()
{
    VkDevice device = m_context->getDevice();

    auto destroyPL = [&](VkPipeline& p, VkPipelineLayout& l) {
        if (p) { vkDestroyPipeline(device, p, nullptr); p = VK_NULL_HANDLE; }
        if (l) { vkDestroyPipelineLayout(device, l, nullptr); l = VK_NULL_HANDLE; }
    };

    destroyPL(m_equirectPipeline, m_equirectPipelineLayout);
    destroyPL(m_irradiancePipeline, m_irradiancePipelineLayout);
    destroyPL(m_prefilterPipeline, m_prefilterPipelineLayout);
    destroyPL(m_brdfPipeline, m_brdfPipelineLayout);
}

// ========================================================================
// 辅助: 更新描述符集中的纹理绑定
// ========================================================================

static void writeDescriptorBinding(VkDevice device, VkDescriptorSet set,
                                   uint32_t binding, VkImageView view,
                                   VkSampler sampler,
                                   VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
{
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = layout;
    imgInfo.imageView = view;
    imgInfo.sampler = sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

// ========================================================================
// generate 主入口
// ========================================================================

void IBLGenerator::generate(VkImageView panoramaView, VkSampler panoramaSampler)
{
    SDL_Log("[IBLGenerator] generate: 开始生成 IBL 贴图...");
    generateCubemap(panoramaView, panoramaSampler);
    generateIrradiance();
    generatePrefiltered();
    m_generated = true;
    SDL_Log("[IBLGenerator] generate: IBL 贴图生成完成");
}

// ========================================================================
// generateCubemap: 全景图 → cubemap (6 面)
// ========================================================================

void IBLGenerator::generateCubemap(VkImageView panoramaView, VkSampler panoramaSampler)
{
    VkDevice device = m_context->getDevice();

    // 绑定全景图到描述符集
    writeDescriptorBinding(device, m_equirectSet, 0, panoramaView, panoramaSampler);

    VkCommandBuffer cmd = m_cmdMgr->beginSingleTimeCommands(device);

    // 转换 cubemap image 到可写状态
    transitionImageLayout(cmd, m_cubemapImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 6);

    std::vector<VkFramebuffer> tempFramebuffers;

    for (uint32_t face = 0; face < 6; face++) {
        VkFramebuffer fb = createFramebuffer(m_hdrRenderPass, m_cubemapFaceViews[face],
                                              CUBEMAP_SIZE, CUBEMAP_SIZE);
        tempFramebuffers.push_back(fb);

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_hdrRenderPass;
        rpBegin.framebuffer = fb;
        rpBegin.renderArea = {{0, 0}, {CUBEMAP_SIZE, CUBEMAP_SIZE}};

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(CUBEMAP_SIZE), static_cast<float>(CUBEMAP_SIZE), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {CUBEMAP_SIZE, CUBEMAP_SIZE}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_equirectPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_equirectPipelineLayout, 0, 1, &m_equirectSet, 0, nullptr);

        IBLPushConstant pc{};
        pc.faceIndex = static_cast<int32_t>(face);
        pc.roughness = 0.0f;
        vkCmdPushConstants(cmd, m_equirectPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(IBLPushConstant), &pc);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // 转换为可采样状态
    transitionImageLayout(cmd, m_cubemapImage,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 6);

    m_cmdMgr->endSingleTimeCommands(device, m_context->getGraphicsQueue(), cmd);

    // GPU 执行完毕后销毁临时 framebuffer
    for (auto fb : tempFramebuffers)
        vkDestroyFramebuffer(device, fb, nullptr);
}

// ========================================================================
// generateIrradiance: cubemap → irradiance map (6 面)
// ========================================================================

void IBLGenerator::generateIrradiance()
{
    VkDevice device = m_context->getDevice();

    // 绑定 cubemap 到 irradiance 描述符集
    writeDescriptorBinding(device, m_irradianceSet, 0, m_cubemapCubeView, m_cubemapSampler);

    VkCommandBuffer cmd = m_cmdMgr->beginSingleTimeCommands(device);

    transitionImageLayout(cmd, m_irradianceImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 6);

    std::vector<VkFramebuffer> tempFramebuffers;

    for (uint32_t face = 0; face < 6; face++) {
        VkFramebuffer fb = createFramebuffer(m_hdrRenderPass, m_irradianceFaceViews[face],
                                              IRRADIANCE_SIZE, IRRADIANCE_SIZE);
        tempFramebuffers.push_back(fb);

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_hdrRenderPass;
        rpBegin.framebuffer = fb;
        rpBegin.renderArea = {{0, 0}, {IRRADIANCE_SIZE, IRRADIANCE_SIZE}};

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(IRRADIANCE_SIZE), static_cast<float>(IRRADIANCE_SIZE), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {IRRADIANCE_SIZE, IRRADIANCE_SIZE}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_irradiancePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_irradiancePipelineLayout, 0, 1, &m_irradianceSet, 0, nullptr);

        IBLPushConstant pc{};
        pc.faceIndex = static_cast<int32_t>(face);
        pc.roughness = 0.0f;
        vkCmdPushConstants(cmd, m_irradiancePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(IBLPushConstant), &pc);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    transitionImageLayout(cmd, m_irradianceImage,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 6);

    m_cmdMgr->endSingleTimeCommands(device, m_context->getGraphicsQueue(), cmd);

    for (auto fb : tempFramebuffers)
        vkDestroyFramebuffer(device, fb, nullptr);
}

// ========================================================================
// generatePrefiltered: cubemap → pre-filtered env map (6面 × 5 mip)
// ========================================================================

void IBLGenerator::generatePrefiltered()
{
    VkDevice device = m_context->getDevice();

    // 绑定 cubemap 到 prefilter 描述符集
    writeDescriptorBinding(device, m_prefilterSet, 0, m_cubemapCubeView, m_cubemapSampler);

    VkCommandBuffer cmd = m_cmdMgr->beginSingleTimeCommands(device);

    transitionImageLayout(cmd, m_prefilteredImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, PREFILTER_MIP_LEVELS, 6);

    std::vector<VkFramebuffer> tempFramebuffers;

    for (uint32_t mip = 0; mip < PREFILTER_MIP_LEVELS; mip++) {
        uint32_t mipSize = PREFILTER_SIZE >> mip;
        float roughness = static_cast<float>(mip) / static_cast<float>(PREFILTER_MIP_LEVELS - 1);

        for (uint32_t face = 0; face < 6; face++) {
            VkImageView faceView = m_prefilteredFaceViews[mip * 6 + face];
            VkFramebuffer fb = createFramebuffer(m_hdrRenderPass, faceView, mipSize, mipSize);
            tempFramebuffers.push_back(fb);

            VkRenderPassBeginInfo rpBegin{};
            rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpBegin.renderPass = m_hdrRenderPass;
            rpBegin.framebuffer = fb;
            rpBegin.renderArea = {{0, 0}, {mipSize, mipSize}};

            vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{0.0f, 0.0f, static_cast<float>(mipSize), static_cast<float>(mipSize), 0.0f, 1.0f};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            VkRect2D scissor{{0, 0}, {mipSize, mipSize}};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_prefilterPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_prefilterPipelineLayout, 0, 1, &m_prefilterSet, 0, nullptr);

            IBLPushConstant pc{};
            pc.faceIndex = static_cast<int32_t>(face);
            pc.roughness = roughness;
            vkCmdPushConstants(cmd, m_prefilterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(IBLPushConstant), &pc);

            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
        }
    }

    transitionImageLayout(cmd, m_prefilteredImage,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        PREFILTER_MIP_LEVELS, 6);

    m_cmdMgr->endSingleTimeCommands(device, m_context->getGraphicsQueue(), cmd);

    for (auto fb : tempFramebuffers)
        vkDestroyFramebuffer(device, fb, nullptr);
}

// ========================================================================
// generateBrdfLut: BRDF 积分 LUT
// ========================================================================

void IBLGenerator::generateBrdfLut()
{
    VkDevice device = m_context->getDevice();

    VkCommandBuffer cmd = m_cmdMgr->beginSingleTimeCommands(device);

    transitionImageLayout(cmd, m_brdfLutImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 1);

    VkFramebuffer fb = createFramebuffer(m_brdfRenderPass, m_brdfLutView, BRDF_LUT_SIZE, BRDF_LUT_SIZE);

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = m_brdfRenderPass;
    rpBegin.framebuffer = fb;
    rpBegin.renderArea = {{0, 0}, {BRDF_LUT_SIZE, BRDF_LUT_SIZE}};

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(BRDF_LUT_SIZE), static_cast<float>(BRDF_LUT_SIZE), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {BRDF_LUT_SIZE, BRDF_LUT_SIZE}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_brdfPipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    transitionImageLayout(cmd, m_brdfLutImage,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1);

    m_cmdMgr->endSingleTimeCommands(device, m_context->getGraphicsQueue(), cmd);

    vkDestroyFramebuffer(device, fb, nullptr);

    SDL_Log("[IBLGenerator] BRDF LUT 生成完成");
}

} // namespace QymEngine
