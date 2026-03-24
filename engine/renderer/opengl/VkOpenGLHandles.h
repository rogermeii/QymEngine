#pragma once

// ============================================================================
// OpenGL 后端 - 内部结构体定义 (GL_ 前缀)
//
// 为避免与 D3D12/D3D11 后端的结构体 ODR 冲突，
// OpenGL 后端使用独立命名的结构体 (GL_Instance, GL_Device, ...)。
// VkOpenGL.cpp 中通过 AS_GL / TO_VK 宏在 VkXxx 句柄与 GL_Xxx* 之间转换。
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>
#ifdef __ANDROID__
#include <GLES3/gl32.h>
#ifndef GL_FILL
#define GL_FILL 0x1B02
#endif
#ifndef GL_LINE
#define GL_LINE 0x1B01
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#else
#include <glad/glad.h>
#endif
#include <SDL.h>
#include <vector>
#include <string>
#include <cstdint>

// ============================================================================
// Instance / PhysicalDevice / Device / Queue
// ============================================================================

struct GL_Instance {
    bool                        initialized = false;
    std::vector<VkPhysicalDevice> physicalDevices;
};

struct GL_PhysicalDevice {
    std::string                 rendererStr;
    std::string                 versionStr;
    VkInstance                  instance = VK_NULL_HANDLE;
};

struct GL_Device {
    SDL_GLContext               glContext = nullptr;
    SDL_Window*                 window = nullptr;
    VkPhysicalDevice            physicalDevice = VK_NULL_HANDLE;
};

struct GL_Queue {
    VkDevice                    device = VK_NULL_HANDLE;
};

// ============================================================================
// Surface / Swapchain
// ============================================================================

struct GL_Surface {
    SDL_Window*                 window = nullptr;
};

struct GL_Swapchain {
    uint32_t                    width = 0;
    uint32_t                    height = 0;
    uint32_t                    imageCount = 0;
    uint32_t                    currentIndex = 0;
    std::vector<VkImage>        imageHandles;
    VkFormat                    format = VK_FORMAT_UNDEFINED;
};

// ============================================================================
// Memory / Buffer / Image
// ============================================================================

struct GL_Memory {
    VkDeviceSize                size = 0;
    void*                       mapped = nullptr;
    uint32_t                    memoryTypeIndex = 0;
};

struct GL_Buffer {
    GLuint                      buffer = 0;
    VkDeviceSize                size = 0;
    VkBufferUsageFlags          usage = 0;
    void*                       mapped = nullptr;
    VkDeviceMemory              boundMemory = VK_NULL_HANDLE;
};

struct GL_Image {
    GLuint                      texture = 0;
    VkFormat                    format = VK_FORMAT_UNDEFINED;
    uint32_t                    width = 0;
    uint32_t                    height = 0;
    uint32_t                    mipLevels = 1;
    VkImageUsageFlags           usage = 0;
    bool                        ownsResource = true;
    VkDeviceMemory              boundMemory = VK_NULL_HANDLE;
    VkImageLayout               currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct GL_ImageView {
    VkImage                     image = VK_NULL_HANDLE;
    VkFormat                    format = VK_FORMAT_UNDEFINED;
    VkImageViewType             viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkImageSubresourceRange     subresourceRange = {};
};

// ============================================================================
// Sampler
// ============================================================================

struct GL_Sampler {
    GLuint                      sampler = 0;
};

// ============================================================================
// Command Pool / Command Buffer
// ============================================================================

struct GL_CommandPool {
    VkDevice                    device = VK_NULL_HANDLE;
};

// CopyImageToBuffer 延迟回读记录
struct GL_PendingReadback {
    GLuint                      srcTexture = 0;    // 非 0 表示纹理回读，0 表示 FBO 回读
    VkBuffer                    dstBuffer = VK_NULL_HANDLE;
    uint32_t                    width = 0;
    uint32_t                    height = 0;
    uint32_t                    texelSize = 0;
    VkDeviceSize                bufferOffset = 0;
    GLenum                      glFormat = GL_RGBA;     // GL 像素格式
    GLenum                      glType = GL_UNSIGNED_BYTE; // GL 像素类型
    uint32_t                    mipLevel = 0;
};

struct GL_CommandBuffer {
    VkDevice                    device = VK_NULL_HANDLE;
    bool                        isRecording = false;
    // 当前绑定状态
    VkRenderPass                currentRenderPass = VK_NULL_HANDLE;
    VkFramebuffer               currentFramebuffer = VK_NULL_HANDLE;
    // 延迟绑定状态
    VkPipeline                  currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet             boundSets[4] = {};
    bool                        stateDirty = false;
    // Push constants buffer
    GLuint                      pushConstantUBO = 0;
    uint8_t                     pushConstantData[128] = {};
    uint32_t                    pushConstantSize = 0;
    // 延迟回读列表
    std::vector<GL_PendingReadback> pendingReadbacks;
    // Index buffer 绑定状态
    GLuint                      boundIndexBuffer = 0;
    GLenum                      indexType = GL_UNSIGNED_INT;
    VkDeviceSize                indexBufferOffset = 0;
    // GLES: 顶点缓冲信息 (用于模拟 glDrawElementsBaseVertex)
    GLuint                      glesVertexBuffers[8] = {};
    GLintptr                    glesVertexOffsets[8] = {};
};

// ============================================================================
// Render Pass / Framebuffer
// ============================================================================

struct GL_RenderPass {
    std::vector<VkAttachmentDescription> attachments;
    bool                        hasDepth = false;
    uint32_t                    colorAttachmentCount = 0;
};

struct GL_Framebuffer {
    GLuint                      fbo = 0;
    std::vector<VkImageView>    attachments;
    uint32_t                    width = 0;
    uint32_t                    height = 0;
    VkRenderPass                renderPass = VK_NULL_HANDLE;
};

// ============================================================================
// Pipeline
// ============================================================================

struct GL_ShaderModule {
    std::string                 glslSource;
    bool                        isImguiReplacement = false;
    // GLES: sampler 名称 → texture unit 映射 (由 fixupGLSL 生成)
    std::vector<std::pair<std::string, int>> samplerUnits;
};

struct GL_PipelineLayout {
    std::vector<VkDescriptorSetLayout>  setLayouts;
    std::vector<VkPushConstantRange>    pushConstantRanges;
    uint32_t                            pushConstSize = 0;
};

struct GL_Pipeline {
    GLuint                      program = 0;
    GLuint                      vao = 0;
    VkPipelineLayout            layout = VK_NULL_HANDLE;
    GLenum                      topology = GL_TRIANGLES;
    uint32_t                    vertexStrides[8] = {};
    uint32_t                    vertexBindingCount = 0;
    // Rasterizer state
    bool                        cullEnable = false;
    GLenum                      cullMode = GL_BACK;
    GLenum                      frontFace = GL_CCW;
    GLenum                      polygonMode = GL_FILL;
    // Depth state
    bool                        depthTestEnable = false;
    bool                        depthWriteEnable = false;
    GLenum                      depthFunc = GL_LESS;
    // Blend state
    bool                        blendEnable = false;
    GLenum                      srcBlend = GL_ONE;
    GLenum                      dstBlend = GL_ZERO;
    GLenum                      blendOp = GL_FUNC_ADD;
    GLenum                      srcBlendAlpha = GL_ONE;
    GLenum                      dstBlendAlpha = GL_ZERO;
    GLenum                      blendOpAlpha = GL_FUNC_ADD;
    uint32_t                    colorWriteMask = 0xF;
    float                       blendConstants[4] = {0.f, 0.f, 0.f, 0.f};
    // Depth bias
    bool                        depthBiasEnable = false;
    float                       depthBiasConstant = 0.f;
    float                       depthBiasSlope = 0.f;
    float                       depthBiasClamp = 0.f;
    // GLES 3.0 顶点属性缓存 (glVertexAttribPointer 需要这些信息)
    uint32_t                    attribCount = 0;
    int                         attribComponents[16] = {};
    GLenum                      attribType[16] = {};
    bool                        attribNormalized[16] = {};
    uint32_t                    attribOffset[16] = {};
    uint32_t                    attribBinding[16] = {};
};

// ============================================================================
// Descriptor
// ============================================================================

struct GL_DescriptorSetLayout {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

struct GL_DescriptorPool {
    VkDevice                    device = VK_NULL_HANDLE;
};

struct GL_DescriptorSet {
    VkDescriptorSetLayout       layout = VK_NULL_HANDLE;
    // UBO 绑定
    GLuint                      uboBuffers[8] = {};
    VkBuffer                    uboVkBuffers[8] = {};
    uint32_t                    uboCount = 0;
    // Texture/Sampler 绑定
    GLuint                      textures[8] = {};
    GLuint                      samplers[8] = {};
    uint32_t                    textureCount = 0;
};

// ============================================================================
// Synchronization
// ============================================================================

struct GL_Fence {
    bool                        signaled = false;
};

struct GL_Semaphore {
    // OpenGL 单线程渲染无需 semaphore
};

// ============================================================================
// Debug
// ============================================================================

struct GL_DebugMessenger {
    PFN_vkDebugUtilsMessengerCallbackEXT    callback = nullptr;
    void*                                   userData = nullptr;
};
