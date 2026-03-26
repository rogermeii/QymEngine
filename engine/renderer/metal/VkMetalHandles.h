#pragma once

// ============================================================================
// Metal 后端 - 内部结构体定义 (MTL_ 前缀)
//
// 为避免与其他后端的结构体 ODR 冲突，
// Metal 后端使用独立命名的结构体 (MTL_Instance, MTL_Device, ...)。
// VkMetal.mm 中通过 AS_MTL / TO_VK 宏在 VkXxx 句柄与 MTL_Xxx* 之间转换。
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#else
// 前向声明 (非 ObjC 编译单元)
typedef void* id;
#endif

#include <SDL.h>
#include <vector>
#include <string>
#include <map>
#include <cstdint>
#include <dispatch/dispatch.h>

// ============================================================================
// Instance / PhysicalDevice / Device / Queue
// ============================================================================

struct MTL_Instance {
    bool                            initialized = false;
    std::vector<VkPhysicalDevice>   physicalDevices;
};

struct MTL_PhysicalDevice {
#ifdef __OBJC__
    id<MTLDevice>                   device = nil;
#else
    id                              device = nullptr;
#endif
    std::string                     deviceName;
    VkInstance                      instance = VK_NULL_HANDLE;
};

struct MTL_Device {
#ifdef __OBJC__
    id<MTLDevice>                   device = nil;
    id<MTLCommandQueue>             commandQueue = nil;
#else
    id                              device = nullptr;
    id                              commandQueue = nullptr;
#endif
    SDL_Window*                     window = nullptr;
    VkPhysicalDevice                physicalDevice = VK_NULL_HANDLE;
};

struct MTL_Queue {
    VkDevice                        device = VK_NULL_HANDLE;
};

// ============================================================================
// Surface / Swapchain
// ============================================================================

struct MTL_Surface {
    SDL_Window*                     window = nullptr;
#ifdef __OBJC__
    CAMetalLayer*                   metalLayer = nil;
#else
    void*                           metalLayer = nullptr;
#endif
};

struct MTL_Swapchain {
    uint32_t                        width = 0;
    uint32_t                        height = 0;
    uint32_t                        imageCount = 0;
    uint32_t                        currentIndex = 0;
    VkFormat                        format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage>            imageHandles;
#ifdef __OBJC__
    CAMetalLayer*                   metalLayer = nil;
    id<CAMetalDrawable>             currentDrawable = nil;
#else
    void*                           metalLayer = nullptr;
    void*                           currentDrawable = nullptr;
#endif
    SDL_MetalView                   metalView = nullptr;
};

// ============================================================================
// Memory / Buffer / Image
// ============================================================================

struct MTL_Memory {
    VkDeviceSize                    size = 0;
    uint32_t                        memoryTypeIndex = 0;
    // 关联的 buffer（由 BindBufferMemory 设置，MapMemory 使用）
    VkBuffer                        boundBuffer = VK_NULL_HANDLE;
    // 关联的 image（由 BindImageMemory 设置）
    VkImage                         boundImage = VK_NULL_HANDLE;
};

struct MTL_Buffer {
#ifdef __OBJC__
    id<MTLBuffer>                   buffer = nil;
#else
    id                              buffer = nullptr;
#endif
    VkDeviceSize                    size = 0;
    VkBufferUsageFlags              usage = 0;
    void*                           mapped = nullptr;
};

struct MTL_Image {
#ifdef __OBJC__
    id<MTLTexture>                  texture = nil;
#else
    id                              texture = nullptr;
#endif
    VkFormat                        format = VK_FORMAT_UNDEFINED;
    uint32_t                        width = 0;
    uint32_t                        height = 0;
    uint32_t                        mipLevels = 1;
    VkImageUsageFlags               usage = 0;
    bool                            ownsResource = true;
};

struct MTL_ImageView {
    VkImage                         image = VK_NULL_HANDLE;
#ifdef __OBJC__
    id<MTLTexture>                  textureView = nil;
#else
    id                              textureView = nullptr;
#endif
    VkFormat                        format = VK_FORMAT_UNDEFINED;
    VkImageViewType                 viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkImageSubresourceRange         subresourceRange = {};
};

// ============================================================================
// Sampler
// ============================================================================

struct MTL_Sampler {
#ifdef __OBJC__
    id<MTLSamplerState>             sampler = nil;
#else
    id                              sampler = nullptr;
#endif
};

// ============================================================================
// Command Pool / Command Buffer
// ============================================================================

struct MTL_CommandPool {
    VkDevice                        device = VK_NULL_HANDLE;
};

struct MTL_CommandBuffer {
    VkDevice                        device = VK_NULL_HANDLE;
#ifdef __OBJC__
    id<MTLCommandBuffer>            commandBuffer = nil;
    id<MTLRenderCommandEncoder>     encoder = nil;
#else
    id                              commandBuffer = nullptr;
    id                              encoder = nullptr;
#endif
    // 当前绑定状态
    VkPipeline                      currentPipeline = VK_NULL_HANDLE;
    VkRenderPass                    currentRenderPass = VK_NULL_HANDLE;
    VkFramebuffer                   currentFramebuffer = VK_NULL_HANDLE;
    VkDescriptorSet                 boundSets[4] = {};
    // Index buffer 绑定状态
#ifdef __OBJC__
    id<MTLBuffer>                   boundIndexBuffer = nil;
#else
    id                              boundIndexBuffer = nullptr;
#endif
    VkIndexType                     indexType = VK_INDEX_TYPE_UINT32;
    VkDeviceSize                    indexBufferOffset = 0;
    // Push constants 缓冲
    uint8_t                         pushConstantData[256] = {};
    uint32_t                        pushConstantSize = 0;
};

// ============================================================================
// Render Pass / Framebuffer
// ============================================================================

struct MTL_RenderPass {
    std::vector<VkAttachmentDescription> attachments;
    bool                            hasDepth = false;
    uint32_t                        colorAttachmentCount = 0;
};

struct MTL_Framebuffer {
    std::vector<VkImageView>        attachments;
    uint32_t                        width = 0;
    uint32_t                        height = 0;
    VkRenderPass                    renderPass = VK_NULL_HANDLE;
};

// ============================================================================
// Pipeline
// ============================================================================

// MSL 资源绑定信息（从 Slang 生成的 MSL 文本中解析）
struct MTL_ResourceBindings {
    // 顶点缓冲 Metal buffer index（= MSL 中最大 buffer(N) + 1，仅 vertex shader）
    uint32_t                        vertexBufferIndex = 0;
    bool                            hasVertexInputs = false;

    // UBO: (desc_set, binding) → MSL buffer index
    std::map<std::pair<uint32_t,uint32_t>, uint32_t>  uboBindings;

    // 纹理: (desc_set, binding) → MSL texture index
    std::map<std::pair<uint32_t,uint32_t>, uint32_t>  textureBindings;

    // 采样器: (desc_set, binding) → MSL sampler index
    std::map<std::pair<uint32_t,uint32_t>, uint32_t>  samplerBindings;

    // Push constants → MSL buffer index
    uint32_t                        pushConstantBufferIndex = UINT32_MAX;

    // 基于变量名的绑定映射（从 MSL 入口函数参数中解析）
    // Slang MSL 中 UBO 参数名格式: frame_N, materialParams_N
    // 纹理参数名格式: shadowMap_texture_N, albedoMap_texture_N
    std::map<std::string, uint32_t> namedBufferBindings;   // varBaseName → metal buffer(N)
    std::map<std::string, uint32_t> namedTextureBindings;  // varBaseName → metal texture(N)
    std::map<std::string, uint32_t> namedSamplerBindings;  // varBaseName → metal sampler(N)
};

struct MTL_ShaderModule {
    std::string                     mslSource;

    // 从 MSL 文本中解析的资源绑定
    MTL_ResourceBindings            bindings;

    // 入口函数名（从 MSL [[vertex]]/[[fragment]] 后解析）
    std::string                     entryPointName;

    // 着色器执行模型: 0=vertex, 4=fragment
    uint32_t                        executionModel = 0;

    // ImGui 着色器标记（SPIR-V 输入，使用内置 MSL 替换）
    bool                            isImguiReplacement = false;
};

struct MTL_PipelineLayout {
    std::vector<VkDescriptorSetLayout>  setLayouts;
    std::vector<VkPushConstantRange>    pushConstantRanges;
    uint32_t                            pushConstSize = 0;
};

struct MTL_Pipeline {
#ifdef __OBJC__
    id<MTLRenderPipelineState>      pipelineState = nil;
    id<MTLDepthStencilState>        depthStencilState = nil;
#else
    id                              pipelineState = nullptr;
    id                              depthStencilState = nullptr;
#endif
    VkPipelineLayout                layout = VK_NULL_HANDLE;
    // 光栅化状态 (Metal 在 encoder 上设置)
    uint32_t                        cullMode = 0;       // MTLCullModeNone
    uint32_t                        frontFace = 0;      // MTLWindingClockwise
    uint32_t                        topology = 3;       // MTLPrimitiveTypeTriangle
    uint32_t                        vertexStrides[8] = {};
    uint32_t                        vertexBindingCount = 0;
    // Depth bias
    bool                            depthBiasEnable = false;
    float                           depthBiasConstant = 0.f;
    float                           depthBiasSlope = 0.f;
    float                           depthBiasClamp = 0.f;

    // 顶点着色器 / 片段着色器的资源绑定映射（从 MSL 解析）
    MTL_ResourceBindings            vsBindings;
    MTL_ResourceBindings            fsBindings;
};

// ============================================================================
// Descriptor
// ============================================================================

struct MTL_DescriptorSetLayout {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

struct MTL_DescriptorPool {
    VkDevice                        device = VK_NULL_HANDLE;
};

struct MTL_DescriptorSet {
    VkDescriptorSetLayout           layout = VK_NULL_HANDLE;
    // UBO 绑定
    VkBuffer                        uboVkBuffers[8] = {};
    VkDeviceSize                    uboOffsets[8] = {};
    uint32_t                        uboCount = 0;
    // Texture/Sampler 绑定
    VkImageView                     imageViews[8] = {};
    VkSampler                       samplers[8] = {};
    uint32_t                        textureCount = 0;
};

// ============================================================================
// Synchronization
// ============================================================================

struct MTL_Fence {
    dispatch_semaphore_t            semaphore = nullptr;
    bool                            signaled = false;
};

struct MTL_Semaphore {
    // Metal 单命令队列渲染无需 semaphore
};

// ============================================================================
// Debug
// ============================================================================

struct MTL_DebugMessenger {
    PFN_vkDebugUtilsMessengerCallbackEXT    callback = nullptr;
    void*                                   userData = nullptr;
};
