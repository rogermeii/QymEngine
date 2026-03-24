#include "renderer/PostProcess.h"
#include "renderer/VulkanContext.h"
#include "renderer/DescriptorLayoutCache.h"
#include "renderer/VkDispatch.h"
#include "asset/ShaderBundle.h"

#include <array>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace QymEngine {

// ========================================================================
// Push constant 结构体 (与着色器一一对应)
// ========================================================================

struct BloomPushConstant {
    float texelSize[2];
    float threshold;
    float intensity;
    int32_t mipLevel;
    int32_t useBrightPass;
};
static_assert(sizeof(BloomPushConstant) == 24, "BloomPushConstant size mismatch");

struct CompositePushConstant {
    float texelSize[2];
    float exposure;
    float bloomIntensity;
    int32_t bloomEnabled;
    int32_t toneMappingEnabled;
    int32_t colorGradingEnabled;
    float contrast;
    float saturation;
    float temperature;
    float tint;
    float brightness;
};
static_assert(sizeof(CompositePushConstant) == 48, "CompositePushConstant size mismatch");

struct FxaaPushConstant {
    float texelSize[2];
    float subpixQuality;
    float edgeThreshold;
    float edgeThresholdMin;
};
static_assert(sizeof(FxaaPushConstant) == 20, "FxaaPushConstant size mismatch");

// ========================================================================
// 着色器变体/路径辅助函数 (与 Renderer.cpp 中的 static 函数保持一致)
// ========================================================================

static std::string ppShaderVariant(const std::string& base) {
    if (vkIsD3D12Backend())
        return base + "_dxil";
    if (vkIsD3D11Backend())
        return base + "_dxbc";
    if (vkIsOpenGLBackend() || vkIsGLESBackend())
        return base + "_glsl";
    return base;
}

static std::string ppShaderBundlePath(const char* bundleBaseName) {
#ifdef __ANDROID__
    return std::string("shaders/postprocess/") + bundleBaseName + ".shaderbundle";
#else
    return std::string(ASSETS_DIR) + "/shaders/postprocess/" + bundleBaseName + ".shaderbundle";
#endif
}

// ========================================================================
// createFullscreenBundlePipeline (与 Renderer.cpp 中的版本完全相同)
// ========================================================================

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

// ========================================================================
// 辅助: 更新描述符集 (单张纹理绑定)
// ========================================================================

static void writeDescriptorBinding(VkDevice device, VkDescriptorSet set,
                                   uint32_t binding, VkImageView view,
                                   VkSampler sampler) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
// 辅助: 创建单张 2D image + memory + view
// ========================================================================

static void createImage2D(VulkanContext& ctx, uint32_t width, uint32_t height,
                          VkFormat format, uint32_t mipLevels,
                          VkImageUsageFlags usage,
                          VkImage& outImage, VkDeviceMemory& outMemory) {
    VkDevice device = ctx.getDevice();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &outImage) != VK_SUCCESS)
        throw std::runtime_error("failed to create post-process image!");

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, outImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = ctx.findMemoryType(memReqs.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate post-process image memory!");

    vkBindImageMemory(device, outImage, outMemory, 0);
}

static VkImageView createImageView2D(VkDevice device, VkImage image, VkFormat format,
                                      uint32_t baseMip, uint32_t mipCount) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = baseMip;
    viewInfo.subresourceRange.levelCount = mipCount;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS)
        throw std::runtime_error("failed to create post-process image view!");
    return view;
}

// ========================================================================
// 辅助: 创建 render pass (color-only, 无深度)
// ========================================================================

static VkRenderPass createColorOnlyRenderPass(VkDevice device, VkFormat format,
                                               VkAttachmentLoadOp loadOp,
                                               VkImageLayout initialLayout,
                                               VkImageLayout finalLayout) {
    VkAttachmentDescription colorAttach{};
    colorAttach.format = format;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = loadOp;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = initialLayout;
    colorAttach.finalLayout = finalLayout;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // 同步依赖
    VkSubpassDependency dependencies[2]{};

    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAttach;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 2;
    rpInfo.pDependencies = dependencies;

    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device, &rpInfo, nullptr, &rp) != VK_SUCCESS)
        throw std::runtime_error("failed to create post-process render pass!");
    return rp;
}

// ========================================================================
// init
// ========================================================================

void PostProcessPipeline::init(VulkanContext& ctx, DescriptorLayoutCache& layoutCache,
                                uint32_t width, uint32_t height) {
    m_context = &ctx;
    m_layoutCache = &layoutCache;
    m_width = width;
    m_height = height;

    VkDevice device = m_context->getDevice();

    // --- 1. 线性采样器 ---
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
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(MAX_BLOOM_MIPS);

        if (vkCreateSampler(device, &samplerInfo, nullptr, &m_linearSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create post-process sampler!");
    }

    // --- 2. 描述符集布局: 2 个 COMBINED_IMAGE_SAMPLER 绑定 ---
    {
        VkDescriptorSetLayoutBinding binding0{};
        binding0.binding = 0;
        binding0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding0.descriptorCount = 1;
        binding0.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding binding1{};
        binding1.binding = 1;
        binding1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding1.descriptorCount = 1;
        binding1.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        m_postProcessSetLayout = m_layoutCache->getOrCreate(device, {binding0, binding1});
    }

    // --- 3. 描述符池 ---
    {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 32;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 16;

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create post-process descriptor pool!");
    }

    // --- 4. 创建 3 个 RenderPass ---
    // downsample: HDR format, loadOp=DONT_CARE, initialLayout=UNDEFINED
    m_bloomDownsampleRenderPass = createColorOnlyRenderPass(
        device, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // upsample: HDR format, loadOp=LOAD, initialLayout=COLOR_ATTACHMENT_OPTIMAL
    m_bloomUpsampleRenderPass = createColorOnlyRenderPass(
        device, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_ATTACHMENT_LOAD_OP_LOAD,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // LDR: UNORM format, loadOp=DONT_CARE, initialLayout=UNDEFINED
    m_ldrRenderPass = createColorOnlyRenderPass(
        device, VK_FORMAT_R8G8B8A8_UNORM,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // --- 5. 创建资源 ---
    createBloomResources();
    createLdrResources();
    createBlackFallback();

    // --- 6. 创建管线 ---
    createPipelines();

    // --- 7. 分配描述符集 ---
    {
        // 分配所有描述符集
        uint32_t totalSets = MAX_BLOOM_MIPS + MAX_BLOOM_MIPS + 2; // downsample + upsample + composite + fxaa
        std::vector<VkDescriptorSetLayout> layouts(totalSets, m_postProcessSetLayout);
        std::vector<VkDescriptorSet> sets(totalSets);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = totalSets;
        allocInfo.pSetLayouts = layouts.data();

        if (vkAllocateDescriptorSets(device, &allocInfo, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate post-process descriptor sets!");

        uint32_t idx = 0;
        for (int i = 0; i < MAX_BLOOM_MIPS; i++)
            m_bloomDownsampleSets[i] = sets[idx++];
        for (int i = 0; i < MAX_BLOOM_MIPS; i++)
            m_bloomUpsampleSets[i] = sets[idx++];
        m_compositeSet = sets[idx++];
        m_fxaaSet = sets[idx++];

        // 用黑色备用纹理初始化所有描述符集
        for (int i = 0; i < MAX_BLOOM_MIPS; i++) {
            writeDescriptorBinding(device, m_bloomDownsampleSets[i], 0, m_blackFallbackView, m_linearSampler);
            writeDescriptorBinding(device, m_bloomDownsampleSets[i], 1, m_blackFallbackView, m_linearSampler);
        }
        for (int i = 0; i < MAX_BLOOM_MIPS; i++) {
            writeDescriptorBinding(device, m_bloomUpsampleSets[i], 0, m_blackFallbackView, m_linearSampler);
            writeDescriptorBinding(device, m_bloomUpsampleSets[i], 1, m_blackFallbackView, m_linearSampler);
        }
        writeDescriptorBinding(device, m_compositeSet, 0, m_blackFallbackView, m_linearSampler);
        writeDescriptorBinding(device, m_compositeSet, 1, m_blackFallbackView, m_linearSampler);
        writeDescriptorBinding(device, m_fxaaSet, 0, m_blackFallbackView, m_linearSampler);
        writeDescriptorBinding(device, m_fxaaSet, 1, m_blackFallbackView, m_linearSampler);
    }
}

// ========================================================================
// createBloomResources
// ========================================================================

void PostProcessPipeline::createBloomResources() {
    VkDevice device = m_context->getDevice();

    // mip 0 = width/2 x height/2, 之后每级减半
    uint32_t mipW = std::max(m_width / 2, 1u);
    uint32_t mipH = std::max(m_height / 2, 1u);

    // 创建 bloom mip chain image
    createImage2D(*m_context, mipW, mipH, VK_FORMAT_R16G16B16A16_SFLOAT,
                  MAX_BLOOM_MIPS,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  m_bloomMipImage, m_bloomMipMemory);

    // 为每个 mip 创建 view 和 framebuffer
    for (int i = 0; i < MAX_BLOOM_MIPS; i++) {
        m_bloomMipViews[i] = createImageView2D(device, m_bloomMipImage,
                                                VK_FORMAT_R16G16B16A16_SFLOAT, i, 1);

        uint32_t fbW = std::max(mipW >> i, 1u);
        uint32_t fbH = std::max(mipH >> i, 1u);

        // Downsample framebuffer
        {
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = m_bloomDownsampleRenderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &m_bloomMipViews[i];
            fbInfo.width = fbW;
            fbInfo.height = fbH;
            fbInfo.layers = 1;

            if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_bloomDownsampleFBs[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create bloom downsample framebuffer!");
        }

        // Upsample framebuffer
        {
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = m_bloomUpsampleRenderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &m_bloomMipViews[i];
            fbInfo.width = fbW;
            fbInfo.height = fbH;
            fbInfo.layers = 1;

            if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_bloomUpsampleFBs[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create bloom upsample framebuffer!");
        }
    }
}

void PostProcessPipeline::destroyBloomResources() {
    VkDevice device = m_context->getDevice();

    for (int i = 0; i < MAX_BLOOM_MIPS; i++) {
        if (m_bloomDownsampleFBs[i]) {
            vkDestroyFramebuffer(device, m_bloomDownsampleFBs[i], nullptr);
            m_bloomDownsampleFBs[i] = VK_NULL_HANDLE;
        }
        if (m_bloomUpsampleFBs[i]) {
            vkDestroyFramebuffer(device, m_bloomUpsampleFBs[i], nullptr);
            m_bloomUpsampleFBs[i] = VK_NULL_HANDLE;
        }
        if (m_bloomMipViews[i]) {
            vkDestroyImageView(device, m_bloomMipViews[i], nullptr);
            m_bloomMipViews[i] = VK_NULL_HANDLE;
        }
    }
    if (m_bloomMipImage) {
        vkDestroyImage(device, m_bloomMipImage, nullptr);
        m_bloomMipImage = VK_NULL_HANDLE;
    }
    if (m_bloomMipMemory) {
        vkFreeMemory(device, m_bloomMipMemory, nullptr);
        m_bloomMipMemory = VK_NULL_HANDLE;
    }
}

// ========================================================================
// createLdrResources
// ========================================================================

void PostProcessPipeline::createLdrResources() {
    VkDevice device = m_context->getDevice();

    // Composite image (LDR)
    createImage2D(*m_context, m_width, m_height, VK_FORMAT_R8G8B8A8_UNORM, 1,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  m_compositeImage, m_compositeMemory);
    m_compositeImageView = createImageView2D(device, m_compositeImage,
                                              VK_FORMAT_R8G8B8A8_UNORM, 0, 1);

    {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = m_ldrRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &m_compositeImageView;
        fbInfo.width = m_width;
        fbInfo.height = m_height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_compositeFramebuffer) != VK_SUCCESS)
            throw std::runtime_error("failed to create composite framebuffer!");
    }

    // FXAA image (LDR)
    createImage2D(*m_context, m_width, m_height, VK_FORMAT_R8G8B8A8_UNORM, 1,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  m_fxaaImage, m_fxaaMemory);
    m_fxaaImageView = createImageView2D(device, m_fxaaImage,
                                         VK_FORMAT_R8G8B8A8_UNORM, 0, 1);

    {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = m_ldrRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &m_fxaaImageView;
        fbInfo.width = m_width;
        fbInfo.height = m_height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_fxaaFramebuffer) != VK_SUCCESS)
            throw std::runtime_error("failed to create FXAA framebuffer!");
    }
}

void PostProcessPipeline::destroyLdrResources() {
    VkDevice device = m_context->getDevice();

    if (m_fxaaFramebuffer) { vkDestroyFramebuffer(device, m_fxaaFramebuffer, nullptr); m_fxaaFramebuffer = VK_NULL_HANDLE; }
    if (m_fxaaImageView)   { vkDestroyImageView(device, m_fxaaImageView, nullptr); m_fxaaImageView = VK_NULL_HANDLE; }
    if (m_fxaaImage)       { vkDestroyImage(device, m_fxaaImage, nullptr); m_fxaaImage = VK_NULL_HANDLE; }
    if (m_fxaaMemory)      { vkFreeMemory(device, m_fxaaMemory, nullptr); m_fxaaMemory = VK_NULL_HANDLE; }

    if (m_compositeFramebuffer) { vkDestroyFramebuffer(device, m_compositeFramebuffer, nullptr); m_compositeFramebuffer = VK_NULL_HANDLE; }
    if (m_compositeImageView)   { vkDestroyImageView(device, m_compositeImageView, nullptr); m_compositeImageView = VK_NULL_HANDLE; }
    if (m_compositeImage)       { vkDestroyImage(device, m_compositeImage, nullptr); m_compositeImage = VK_NULL_HANDLE; }
    if (m_compositeMemory)      { vkFreeMemory(device, m_compositeMemory, nullptr); m_compositeMemory = VK_NULL_HANDLE; }
}

// ========================================================================
// createBlackFallback (1x1 黑色 HDR 纹理)
// ========================================================================

void PostProcessPipeline::createBlackFallback() {
    VkDevice device = m_context->getDevice();

    // 创建 1x1 R16G16B16A16_SFLOAT 图像
    createImage2D(*m_context, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, 1,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  m_blackFallbackImage, m_blackFallbackMemory);

    m_blackFallbackView = createImageView2D(device, m_blackFallbackImage,
                                             VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1);

    // 通过 staging buffer + copy 将图像设为黑色并转到 SHADER_READ_ONLY
    // R16G16B16A16_SFLOAT = 8 bytes per pixel
    constexpr VkDeviceSize pixelSize = 8;
    uint16_t blackPixel[4] = {0, 0, 0, 0}; // half-float 0.0

    // 创建 staging buffer
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = pixelSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = m_context->findMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* mapped = nullptr;
        vkMapMemory(device, stagingMemory, 0, pixelSize, 0, &mapped);
        std::memcpy(mapped, blackPixel, pixelSize);
        vkUnmapMemory(device, stagingMemory);
    }

    // 临时 command buffer
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = m_context->findQueueFamilies(m_context->getPhysicalDevice()).graphicsFamily.value();
        vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.commandPool = cmdPool;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmd);
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // UNDEFINED -> TRANSFER_DST_OPTIMAL
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_blackFallbackImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Copy staging buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {1, 1, 1};

    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_blackFallbackImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST -> SHADER_READ_ONLY_OPTIMAL
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_blackFallbackImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_context->getGraphicsQueue());

    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
}

void PostProcessPipeline::destroyBlackFallback() {
    VkDevice device = m_context->getDevice();
    if (m_blackFallbackView)   { vkDestroyImageView(device, m_blackFallbackView, nullptr); m_blackFallbackView = VK_NULL_HANDLE; }
    if (m_blackFallbackImage)  { vkDestroyImage(device, m_blackFallbackImage, nullptr); m_blackFallbackImage = VK_NULL_HANDLE; }
    if (m_blackFallbackMemory) { vkFreeMemory(device, m_blackFallbackMemory, nullptr); m_blackFallbackMemory = VK_NULL_HANDLE; }
}

// ========================================================================
// createPipelines
// ========================================================================

void PostProcessPipeline::createPipelines() {
    VkDevice device = m_context->getDevice();
    std::string variant = ppShaderVariant("default");

    // Bloom downsample: 不需要混合
    {
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(BloomPushConstant);

        createFullscreenBundlePipeline(
            device,
            m_bloomDownsampleRenderPass,
            {m_postProcessSetLayout},
            ppShaderBundlePath("BloomDownsample"),
            variant,
            false, false, false,
            m_bloomDownsamplePipeline,
            m_bloomDownsampleLayout,
            "bloom_downsample",
            &pcRange);
    }

    // Bloom upsample: 加法混合 (src=ONE, dst=ONE)
    {
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(BloomPushConstant);

        VkPipelineColorBlendAttachmentState additiveBlend{};
        additiveBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        additiveBlend.blendEnable = VK_TRUE;
        additiveBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        additiveBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        additiveBlend.colorBlendOp = VK_BLEND_OP_ADD;
        additiveBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        additiveBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        additiveBlend.alphaBlendOp = VK_BLEND_OP_ADD;

        createFullscreenBundlePipeline(
            device,
            m_bloomUpsampleRenderPass,
            {m_postProcessSetLayout},
            ppShaderBundlePath("BloomUpsample"),
            variant,
            false, false, false,
            m_bloomUpsamplePipeline,
            m_bloomUpsampleLayout,
            "bloom_upsample",
            &pcRange,
            &additiveBlend);
    }

    // Composite: 不需要混合
    {
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(CompositePushConstant);

        createFullscreenBundlePipeline(
            device,
            m_ldrRenderPass,
            {m_postProcessSetLayout},
            ppShaderBundlePath("Composite"),
            variant,
            false, false, false,
            m_compositePipeline,
            m_compositeLayout,
            "composite",
            &pcRange);
    }

    // FXAA: 不需要混合
    {
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(FxaaPushConstant);

        createFullscreenBundlePipeline(
            device,
            m_ldrRenderPass,
            {m_postProcessSetLayout},
            ppShaderBundlePath("FXAA"),
            variant,
            false, false, false,
            m_fxaaPipeline,
            m_fxaaLayout,
            "fxaa",
            &pcRange);
    }
}

void PostProcessPipeline::destroyPipelines() {
    VkDevice device = m_context->getDevice();

    if (m_fxaaPipeline)       { vkDestroyPipeline(device, m_fxaaPipeline, nullptr); m_fxaaPipeline = VK_NULL_HANDLE; }
    if (m_fxaaLayout)         { vkDestroyPipelineLayout(device, m_fxaaLayout, nullptr); m_fxaaLayout = VK_NULL_HANDLE; }
    if (m_compositePipeline)  { vkDestroyPipeline(device, m_compositePipeline, nullptr); m_compositePipeline = VK_NULL_HANDLE; }
    if (m_compositeLayout)    { vkDestroyPipelineLayout(device, m_compositeLayout, nullptr); m_compositeLayout = VK_NULL_HANDLE; }
    if (m_bloomUpsamplePipeline)  { vkDestroyPipeline(device, m_bloomUpsamplePipeline, nullptr); m_bloomUpsamplePipeline = VK_NULL_HANDLE; }
    if (m_bloomUpsampleLayout)    { vkDestroyPipelineLayout(device, m_bloomUpsampleLayout, nullptr); m_bloomUpsampleLayout = VK_NULL_HANDLE; }
    if (m_bloomDownsamplePipeline) { vkDestroyPipeline(device, m_bloomDownsamplePipeline, nullptr); m_bloomDownsamplePipeline = VK_NULL_HANDLE; }
    if (m_bloomDownsampleLayout)   { vkDestroyPipelineLayout(device, m_bloomDownsampleLayout, nullptr); m_bloomDownsampleLayout = VK_NULL_HANDLE; }
}

// ========================================================================
// execute
// ========================================================================

void PostProcessPipeline::execute(VkCommandBuffer cmd, VkImageView sceneHDR,
                                   const PostProcessSettings& settings) {
    VkImageView bloomResult = m_blackFallbackView;
    if (settings.bloomEnabled) {
        executeBloom(cmd, sceneHDR, settings);
        bloomResult = m_bloomMipViews[0];
    }
    executeComposite(cmd, sceneHDR, bloomResult, settings);
    if (settings.fxaaEnabled) {
        executeFxaa(cmd, settings);
    }
}

// ========================================================================
// executeBloom
// ========================================================================

void PostProcessPipeline::executeBloom(VkCommandBuffer cmd, VkImageView sceneHDR,
                                        const PostProcessSettings& settings) {
    VkDevice device = m_context->getDevice();
    int mipCount = std::clamp(settings.bloomMipCount, 1, MAX_BLOOM_MIPS);

    uint32_t mip0W = std::max(m_width / 2, 1u);
    uint32_t mip0H = std::max(m_height / 2, 1u);

    // === 降采样链 ===

    // 第一步: scene -> mip 0 (bright extract)
    {
        uint32_t fbW = mip0W;
        uint32_t fbH = mip0H;

        // 更新描述符: 输入是 sceneHDR
        writeDescriptorBinding(device, m_bloomDownsampleSets[0], 0, sceneHDR, m_linearSampler);

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_bloomDownsampleRenderPass;
        rpBegin.framebuffer = m_bloomDownsampleFBs[0];
        rpBegin.renderArea = {{0, 0}, {fbW, fbH}};

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomDownsamplePipeline);

        VkViewport viewport{};
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = static_cast<float>(fbW);
        viewport.height = static_cast<float>(fbH);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor = {{0, 0}, {fbW, fbH}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_bloomDownsampleLayout, 0, 1,
                                &m_bloomDownsampleSets[0], 0, nullptr);

        BloomPushConstant pc{};
        pc.texelSize[0] = 1.0f / static_cast<float>(m_width);
        pc.texelSize[1] = 1.0f / static_cast<float>(m_height);
        pc.threshold = settings.bloomThreshold;
        pc.intensity = 1.0f;  // bright extract 不缩放，仅做阈值提取
        pc.mipLevel = 0;
        pc.useBrightPass = 1;

        vkCmdPushConstants(cmd, m_bloomDownsampleLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(BloomPushConstant), &pc);

        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // 后续降采样: mip i -> mip i+1
    for (int i = 1; i < mipCount; i++) {
        uint32_t srcW = std::max(mip0W >> (i - 1), 1u);
        uint32_t srcH = std::max(mip0H >> (i - 1), 1u);
        uint32_t fbW = std::max(mip0W >> i, 1u);
        uint32_t fbH = std::max(mip0H >> i, 1u);

        // 更新描述符: 输入是前一个 mip (已经是 SHADER_READ_ONLY 布局, 由 renderpass finalLayout 保证)
        writeDescriptorBinding(device, m_bloomDownsampleSets[i], 0, m_bloomMipViews[i - 1], m_linearSampler);

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_bloomDownsampleRenderPass;
        rpBegin.framebuffer = m_bloomDownsampleFBs[i];
        rpBegin.renderArea = {{0, 0}, {fbW, fbH}};

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomDownsamplePipeline);

        VkViewport viewport{};
        viewport.width = static_cast<float>(fbW);
        viewport.height = static_cast<float>(fbH);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor = {{0, 0}, {fbW, fbH}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_bloomDownsampleLayout, 0, 1,
                                &m_bloomDownsampleSets[i], 0, nullptr);

        BloomPushConstant pc{};
        pc.texelSize[0] = 1.0f / static_cast<float>(srcW);
        pc.texelSize[1] = 1.0f / static_cast<float>(srcH);
        pc.threshold = settings.bloomThreshold;
        pc.intensity = 1.0f;  // downsample 不缩放，仅做模糊降采样
        pc.mipLevel = i;
        pc.useBrightPass = 0;

        vkCmdPushConstants(cmd, m_bloomDownsampleLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(BloomPushConstant), &pc);

        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // === 升采样链 (反向: mip i+1 -> mip i, 加法混合) ===
    for (int i = mipCount - 2; i >= 0; i--) {
        uint32_t fbW = std::max(mip0W >> i, 1u);
        uint32_t fbH = std::max(mip0H >> i, 1u);

        // upsample renderpass 的 initialLayout 是 COLOR_ATTACHMENT_OPTIMAL,
        // 所以需要先将目标 mip 从 SHADER_READ_ONLY 转到 COLOR_ATTACHMENT
        {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = m_bloomMipImage;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = static_cast<uint32_t>(i);
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // 输入是下一级 mip (已经是 SHADER_READ_ONLY)
        writeDescriptorBinding(device, m_bloomUpsampleSets[i], 0, m_bloomMipViews[i + 1], m_linearSampler);

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_bloomUpsampleRenderPass;
        rpBegin.framebuffer = m_bloomUpsampleFBs[i];
        rpBegin.renderArea = {{0, 0}, {fbW, fbH}};

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomUpsamplePipeline);

        VkViewport viewport{};
        viewport.width = static_cast<float>(fbW);
        viewport.height = static_cast<float>(fbH);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor = {{0, 0}, {fbW, fbH}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_bloomUpsampleLayout, 0, 1,
                                &m_bloomUpsampleSets[i], 0, nullptr);

        // upsample 输入的 texel size 基于源 mip (i+1) 的尺寸
        uint32_t srcW = std::max(mip0W >> (i + 1), 1u);
        uint32_t srcH = std::max(mip0H >> (i + 1), 1u);

        BloomPushConstant pc{};
        pc.texelSize[0] = 1.0f / static_cast<float>(srcW);
        pc.texelSize[1] = 1.0f / static_cast<float>(srcH);
        pc.threshold = settings.bloomThreshold;
        pc.intensity = 1.0f;  // upsample 固定为 1.0，最终缩放在 Composite 中用 bloomIntensity
        pc.mipLevel = i;
        pc.useBrightPass = 0;

        vkCmdPushConstants(cmd, m_bloomUpsampleLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(BloomPushConstant), &pc);

        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        // renderpass finalLayout 会将其转回 SHADER_READ_ONLY_OPTIMAL
    }
}

// ========================================================================
// executeComposite
// ========================================================================

void PostProcessPipeline::executeComposite(VkCommandBuffer cmd, VkImageView sceneHDR,
                                            VkImageView bloomTexture,
                                            const PostProcessSettings& settings) {
    VkDevice device = m_context->getDevice();

    // 更新描述符: binding 0 = sceneHDR, binding 1 = bloomTexture
    writeDescriptorBinding(device, m_compositeSet, 0, sceneHDR, m_linearSampler);
    writeDescriptorBinding(device, m_compositeSet, 1, bloomTexture, m_linearSampler);

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = m_ldrRenderPass;
    rpBegin.framebuffer = m_compositeFramebuffer;
    rpBegin.renderArea = {{0, 0}, {m_width, m_height}};

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_compositeLayout, 0, 1,
                            &m_compositeSet, 0, nullptr);

    CompositePushConstant pc{};
    pc.texelSize[0] = 1.0f / static_cast<float>(m_width);
    pc.texelSize[1] = 1.0f / static_cast<float>(m_height);
    pc.exposure = settings.exposure;
    pc.bloomIntensity = settings.bloomIntensity;
    pc.bloomEnabled = settings.bloomEnabled ? 1 : 0;
    pc.toneMappingEnabled = settings.toneMappingEnabled ? 1 : 0;
    pc.colorGradingEnabled = settings.colorGradingEnabled ? 1 : 0;
    pc.contrast = settings.contrast;
    pc.saturation = settings.saturation;
    pc.temperature = settings.temperature;
    pc.tint = settings.tint;
    pc.brightness = settings.brightness;

    vkCmdPushConstants(cmd, m_compositeLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(CompositePushConstant), &pc);

    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

// ========================================================================
// executeFxaa
// ========================================================================

void PostProcessPipeline::executeFxaa(VkCommandBuffer cmd, const PostProcessSettings& settings) {
    VkDevice device = m_context->getDevice();

    // 更新描述符: binding 0 = composite output
    writeDescriptorBinding(device, m_fxaaSet, 0, m_compositeImageView, m_linearSampler);

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = m_ldrRenderPass;
    rpBegin.framebuffer = m_fxaaFramebuffer;
    rpBegin.renderArea = {{0, 0}, {m_width, m_height}};

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_fxaaLayout, 0, 1,
                            &m_fxaaSet, 0, nullptr);

    FxaaPushConstant pc{};
    pc.texelSize[0] = 1.0f / static_cast<float>(m_width);
    pc.texelSize[1] = 1.0f / static_cast<float>(m_height);
    pc.subpixQuality = settings.fxaaSubpixQuality;
    pc.edgeThreshold = settings.fxaaEdgeThreshold;
    pc.edgeThresholdMin = settings.fxaaEdgeThresholdMin;

    vkCmdPushConstants(cmd, m_fxaaLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(FxaaPushConstant), &pc);

    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

// ========================================================================
// getFinalImage / getFinalImageView
// ========================================================================

VkImage PostProcessPipeline::getFinalImage(const PostProcessSettings& settings) const {
    if (settings.fxaaEnabled)
        return m_fxaaImage;
    return m_compositeImage;
}

VkImageView PostProcessPipeline::getFinalImageView(const PostProcessSettings& settings) const {
    if (settings.fxaaEnabled)
        return m_fxaaImageView;
    return m_compositeImageView;
}

// ========================================================================
// resize
// ========================================================================

void PostProcessPipeline::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height)
        return;

    VkDevice device = m_context->getDevice();
    vkDeviceWaitIdle(device);

    m_width = width;
    m_height = height;

    // 销毁尺寸相关资源
    destroyBloomResources();
    destroyLdrResources();

    // 销毁并重建描述符池 (dispatch 表中没有 vkResetDescriptorPool)
    if (m_descriptorPool) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 32;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 16;

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
            throw std::runtime_error("failed to recreate post-process descriptor pool!");
    }

    // 重建
    createBloomResources();
    createLdrResources();

    // 重新分配描述符集
    {
        uint32_t totalSets = MAX_BLOOM_MIPS + MAX_BLOOM_MIPS + 2;
        std::vector<VkDescriptorSetLayout> layouts(totalSets, m_postProcessSetLayout);
        std::vector<VkDescriptorSet> sets(totalSets);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = totalSets;
        allocInfo.pSetLayouts = layouts.data();

        if (vkAllocateDescriptorSets(device, &allocInfo, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("failed to re-allocate post-process descriptor sets!");

        uint32_t idx = 0;
        for (int i = 0; i < MAX_BLOOM_MIPS; i++)
            m_bloomDownsampleSets[i] = sets[idx++];
        for (int i = 0; i < MAX_BLOOM_MIPS; i++)
            m_bloomUpsampleSets[i] = sets[idx++];
        m_compositeSet = sets[idx++];
        m_fxaaSet = sets[idx++];

        // 用黑色备用纹理初始化
        for (int i = 0; i < MAX_BLOOM_MIPS; i++) {
            writeDescriptorBinding(device, m_bloomDownsampleSets[i], 0, m_blackFallbackView, m_linearSampler);
            writeDescriptorBinding(device, m_bloomDownsampleSets[i], 1, m_blackFallbackView, m_linearSampler);
        }
        for (int i = 0; i < MAX_BLOOM_MIPS; i++) {
            writeDescriptorBinding(device, m_bloomUpsampleSets[i], 0, m_blackFallbackView, m_linearSampler);
            writeDescriptorBinding(device, m_bloomUpsampleSets[i], 1, m_blackFallbackView, m_linearSampler);
        }
        writeDescriptorBinding(device, m_compositeSet, 0, m_blackFallbackView, m_linearSampler);
        writeDescriptorBinding(device, m_compositeSet, 1, m_blackFallbackView, m_linearSampler);
        writeDescriptorBinding(device, m_fxaaSet, 0, m_blackFallbackView, m_linearSampler);
        writeDescriptorBinding(device, m_fxaaSet, 1, m_blackFallbackView, m_linearSampler);
    }
}

// ========================================================================
// reloadShaders
// ========================================================================

void PostProcessPipeline::reloadShaders() {
    VkDevice device = m_context->getDevice();
    vkDeviceWaitIdle(device);
    destroyPipelines();
    createPipelines();
}

// ========================================================================
// destroy
// ========================================================================

void PostProcessPipeline::destroy() {
    if (!m_context) return;

    VkDevice device = m_context->getDevice();
    vkDeviceWaitIdle(device);

    destroyPipelines();
    destroyBloomResources();
    destroyLdrResources();
    destroyBlackFallback();

    if (m_descriptorPool) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    // m_postProcessSetLayout 由 DescriptorLayoutCache 管理，不在此销毁

    if (m_ldrRenderPass) {
        vkDestroyRenderPass(device, m_ldrRenderPass, nullptr);
        m_ldrRenderPass = VK_NULL_HANDLE;
    }
    if (m_bloomUpsampleRenderPass) {
        vkDestroyRenderPass(device, m_bloomUpsampleRenderPass, nullptr);
        m_bloomUpsampleRenderPass = VK_NULL_HANDLE;
    }
    if (m_bloomDownsampleRenderPass) {
        vkDestroyRenderPass(device, m_bloomDownsampleRenderPass, nullptr);
        m_bloomDownsampleRenderPass = VK_NULL_HANDLE;
    }
    if (m_linearSampler) {
        vkDestroySampler(device, m_linearSampler, nullptr);
        m_linearSampler = VK_NULL_HANDLE;
    }

    m_context = nullptr;
    m_layoutCache = nullptr;
    m_width = 0;
    m_height = 0;
}

} // namespace QymEngine
