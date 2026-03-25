#include "renderer/Pipeline.h"
#include "renderer/VkDispatch.h"
#include "renderer/Buffer.h"
#include <json.hpp>
#include <stdexcept>

using json = nlohmann::json;

namespace QymEngine {

// --- ShaderReflectionData ---

static VkDescriptorType stringToDescriptorType(const std::string& type) {
    if (type == "uniformBuffer") return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (type == "combinedImageSampler") return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (type == "storageBuffer") return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (type == "sampler") return VK_DESCRIPTOR_TYPE_SAMPLER;
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

static VkShaderStageFlags stageStringsToFlags(const std::vector<std::string>& stages) {
    VkShaderStageFlags flags = 0;
    for (auto& s : stages) {
        if (s == "vertex") flags |= VK_SHADER_STAGE_VERTEX_BIT;
        if (s == "fragment") flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        if (s == "compute") flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return flags;
}

bool ShaderReflectionData::loadFromString(const std::string& jsonStr) {
    try {
        json j = json::parse(jsonStr, nullptr, false);
        if (j.is_discarded()) return false;

        sets.clear();
        pushConstants.clear();

        if (j.contains("sets")) {
            for (auto& setJson : j["sets"]) {
                uint32_t setIdx = setJson["set"].get<uint32_t>();
                for (auto& bJson : setJson["bindings"]) {
                    ReflectedBinding rb;
                    rb.set = setIdx;
                    rb.binding = bJson["binding"].get<uint32_t>();
                    rb.name = bJson.value("name", "");
                    rb.type = bJson.value("type", "uniformBuffer");
                    rb.size = bJson.value("size", 0u);
                    for (auto& s : bJson["stages"])
                        rb.stages.push_back(s.get<std::string>());
                    // Parse members
                    if (bJson.contains("members")) {
                        for (auto& mJson : bJson["members"]) {
                            ReflectedMember rm;
                            rm.name = mJson.value("name", "");
                            rm.type = mJson.value("type", "");
                            rm.offset = mJson.value("offset", 0u);
                            rm.size = mJson.value("size", 0u);
                            rb.members.push_back(rm);
                        }
                    }
                    sets[setIdx].push_back(rb);
                }
            }
        }

        if (j.contains("pushConstants")) {
            for (auto& pcJson : j["pushConstants"]) {
                ReflectedPushConstant pc;
                pc.offset = pcJson["offset"].get<uint32_t>();
                pc.size = pcJson["size"].get<uint32_t>();
                for (auto& s : pcJson["stages"])
                    pc.stages.push_back(s.get<std::string>());
                // Parse members
                if (pcJson.contains("members")) {
                    for (auto& mJson : pcJson["members"]) {
                        ReflectedMember rm;
                        rm.name = mJson.value("name", "");
                        rm.type = mJson.value("type", "");
                        rm.offset = mJson.value("offset", 0u);
                        rm.size = mJson.value("size", 0u);
                        pc.members.push_back(rm);
                    }
                }
                pushConstants.push_back(pc);
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

std::vector<VkDescriptorSetLayoutBinding> ShaderReflectionData::buildBindings(uint32_t setIndex) const {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    auto it = sets.find(setIndex);
    if (it != sets.end()) {
        for (auto& rb : it->second) {
            VkDescriptorSetLayoutBinding lb{};
            lb.binding = rb.binding;
            lb.descriptorType = stringToDescriptorType(rb.type);
            lb.descriptorCount = 1;
            lb.stageFlags = stageStringsToFlags(rb.stages);
            lb.pImmutableSamplers = nullptr;
            bindings.push_back(lb);
        }
    }
    return bindings;
}

std::vector<VkPushConstantRange> ShaderReflectionData::createPushConstantRanges() const {
    std::vector<VkPushConstantRange> ranges;
    for (auto& pc : pushConstants) {
        VkPushConstantRange range{};
        range.stageFlags = stageStringsToFlags(pc.stages);
        range.offset = pc.offset;
        // Use at least sizeof(PushConstantData) to allow the engine to push
        // the full struct (which may include padding beyond what shader declares)
        uint32_t minSize = static_cast<uint32_t>(sizeof(PushConstantData));
        range.size = (pc.size < minSize) ? minSize : pc.size;
        ranges.push_back(range);
    }
    return ranges;
}

// --- Pipeline ---

VkShaderModule Pipeline::createShaderModule(VkDevice device, const std::vector<char>& code)
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
        throw std::runtime_error("failed to create shader module!");

    return shaderModule;
}

void Pipeline::createFromMemory(VkDevice device, VkRenderPass renderPass,
                      VkExtent2D extent, DescriptorLayoutCache& layoutCache,
                      VkPolygonMode polygonMode,
                      const std::vector<char>& vertSpv,
                      const std::vector<char>& fragSpv,
                      const std::string& reflectJson,
                      VkDescriptorSetLayout perFrameLayoutOverride)
{
    if (!m_reflection.loadFromString(reflectJson)) {
        throw std::runtime_error("Failed to parse reflection JSON from ShaderBundle");
    }

    // Build layouts from reflection using cache
    m_setLayouts.clear();
    uint32_t maxSet = 0;
    for (auto& [s, _] : m_reflection.sets)
        if (s > maxSet) maxSet = s;

    for (uint32_t setIdx = 0; setIdx <= maxSet; setIdx++) {
        // Set 0 可选覆盖：引擎的 perFrameLayout 包含全部 binding（含 IBL），
        // 但着色器编译器可能优化掉未引用的 binding，导致反射结果不完整。
        if (setIdx == 0 && perFrameLayoutOverride != VK_NULL_HANDLE) {
            m_setLayouts.push_back(perFrameLayoutOverride);
            continue;
        }
        auto bindings = m_reflection.buildBindings(setIdx);
        VkDescriptorSetLayout layout = layoutCache.getOrCreate(device, bindings);
        m_setLayouts.push_back(layout);
    }

    auto pcRanges = m_reflection.createPushConstantRanges();

    VkShaderModule vertModule = createShaderModule(device, vertSpv);
    VkShaderModule fragModule = createShaderModule(device, fragSpv);

    createPipelineCommon(device, renderPass, extent, polygonMode,
                         vertModule, fragModule, m_setLayouts, pcRanges);

    vkDestroyShaderModule(device, fragModule, nullptr);
    vkDestroyShaderModule(device, vertModule, nullptr);
}

void Pipeline::createWithLayoutsFromMemory(VkDevice device, VkRenderPass renderPass,
                      const std::vector<VkDescriptorSetLayout>& descriptorSetLayouts,
                      VkExtent2D extent,
                      const std::vector<VkPushConstantRange>& pushConstantRanges,
                      VkPolygonMode polygonMode,
                      const std::vector<char>& vertSpv,
                      const std::vector<char>& fragSpv,
                      const std::string& reflectJson)
{
    m_reflection.loadFromString(reflectJson);
    m_setLayouts = descriptorSetLayouts;

    VkShaderModule vertModule = createShaderModule(device, vertSpv);
    VkShaderModule fragModule = createShaderModule(device, fragSpv);

    createPipelineCommon(device, renderPass, extent, polygonMode,
                         vertModule, fragModule,
                         descriptorSetLayouts, pushConstantRanges);

    vkDestroyShaderModule(device, fragModule, nullptr);
    vkDestroyShaderModule(device, vertModule, nullptr);
}

void Pipeline::createPipelineCommon(VkDevice device, VkRenderPass renderPass,
                                     VkExtent2D extent, VkPolygonMode polygonMode,
                                     VkShaderModule vertModule, VkShaderModule fragModule,
                                     const std::vector<VkDescriptorSetLayout>& layouts,
                                     const std::vector<VkPushConstantRange>& pcRanges)
{
    VkPipelineShaderStageCreateInfo vertStageInfo{};
    vertStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStageInfo.module = vertModule;
    vertStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragStageInfo{};
    fragStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragModule;
    fragStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertStageInfo, fragStageInfo};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    auto bindingDescription = Vertex::getBindingDescription();
    auto attributeDescriptions = Vertex::getAttributeDescriptions();
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = polygonMode;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (polygonMode == VK_POLYGON_MODE_LINE) {
        rasterizer.depthBiasEnable = VK_TRUE;
        rasterizer.depthBiasConstantFactor = -1.0f;
        rasterizer.depthBiasSlopeFactor = -1.0f;
    } else {
        rasterizer.depthBiasEnable = VK_FALSE;
    }
    rasterizer.depthBiasClamp = 0.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(layouts.size());
    pipelineLayoutInfo.pSetLayouts = layouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pcRanges.size());
    pipelineLayoutInfo.pPushConstantRanges = pcRanges.data();

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create pipeline layout!");

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline) != VK_SUCCESS)
        throw std::runtime_error("failed to create graphics pipeline!");
}

void Pipeline::cleanup(VkDevice device)
{
    vkDestroyPipeline(device, m_graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    // Layouts are owned by DescriptorLayoutCache, not destroyed here
    m_setLayouts.clear();
    m_graphicsPipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
}

} // namespace QymEngine
