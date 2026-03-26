#pragma once

// ============================================================================
// D3D11 后端 - 内部结构体定义 (D11_ 前缀)
//
// 为避免与 D3D12 后端的 VkXxx_T 结构体 ODR 冲突，
// D3D11 后端使用独立命名的结构体 (D11_Instance, D11_Device, ...)。
// VkD3D11.cpp 中通过 AS_D11 / TO_VK 宏在 VkXxx 句柄与 D11_Xxx* 之间转换。
// ============================================================================

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstdint>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

using Microsoft::WRL::ComPtr;

// ============================================================================
// Instance / PhysicalDevice / Device / Queue
// ============================================================================

struct D11_Instance {
    ComPtr<IDXGIFactory4>   factory;
    bool                    debugEnabled = false;
    std::vector<VkPhysicalDevice> physicalDevices;
};

struct D11_PhysicalDevice {
    ComPtr<IDXGIAdapter1>   adapter;
    DXGI_ADAPTER_DESC1      adapterDesc = {};
    VkInstance              instance = VK_NULL_HANDLE;
};

struct D11_Device {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    immediateContext;
    ComPtr<ID3D11InfoQueue>        infoQueue;
    VkPhysicalDevice               physicalDevice = VK_NULL_HANDLE;
    D3D_FEATURE_LEVEL              featureLevel = D3D_FEATURE_LEVEL_11_0;
};

struct D11_Queue {
    VkDevice                    device = VK_NULL_HANDLE;
    uint32_t                    familyIndex = 0;
    uint32_t                    queueIndex = 0;
};

// ============================================================================
// Surface / Swapchain
// ============================================================================

struct D11_Surface {
    HWND        hwnd = nullptr;
    HINSTANCE   hinstance = nullptr;
};

struct D11_Swapchain {
    ComPtr<IDXGISwapChain1>                 swapChain;
    std::vector<ComPtr<ID3D11Texture2D>>    images;
    std::vector<VkImage>                    imageHandles;
    std::vector<ComPtr<ID3D11RenderTargetView>> rtvs;
    DXGI_FORMAT                             format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t                                width = 0;
    uint32_t                                height = 0;
    uint32_t                                imageCount = 0;
    uint32_t                                currentIndex = 0;
};

// ============================================================================
// Memory / Buffer / Image
// ============================================================================

struct D11_Memory {
    D3D11_USAGE     d3dUsage = D3D11_USAGE_DEFAULT;
    VkDeviceSize    size = 0;
    void*           mapped = nullptr;
    uint32_t        memoryTypeIndex = 0;
    // D3D11 不需要独立的 resource, buffer/image 自带
};

struct D11_Buffer {
    ComPtr<ID3D11Buffer>    buffer;
    VkDeviceSize            size = 0;
    VkBufferUsageFlags      usage = 0;
    D3D11_USAGE             d3dUsage = D3D11_USAGE_DEFAULT;
    void*                   mapped = nullptr;
    VkDeviceMemory          boundMemory = VK_NULL_HANDLE;
};

struct D11_Image {
    ComPtr<ID3D11Texture2D>     texture;
    VkFormat                    format = VK_FORMAT_UNDEFINED;
    VkImageLayout               currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t                    width = 0;
    uint32_t                    height = 0;
    uint32_t                    mipLevels = 1;
    uint32_t                    arrayLayers = 1;      // 6 for cubemap
    VkImageUsageFlags           usage = 0;
    bool                        ownsResource = true;
    VkDeviceMemory              boundMemory = VK_NULL_HANDLE;
};

struct D11_ImageView {
    VkImage                             image = VK_NULL_HANDLE;
    VkFormat                            format = VK_FORMAT_UNDEFINED;
    VkImageViewType                     viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkImageSubresourceRange             subresourceRange = {};
    ComPtr<ID3D11RenderTargetView>      rtv;
    ComPtr<ID3D11DepthStencilView>      dsv;
    ComPtr<ID3D11ShaderResourceView>    srv;
    bool                                hasRtv = false;
    bool                                hasDsv = false;
    bool                                hasSrv = false;
};

// ============================================================================
// Sampler
// ============================================================================

struct D11_Sampler {
    ComPtr<ID3D11SamplerState>  sampler;
};

// ============================================================================
// Command Pool / Command Buffer
// ============================================================================

struct D11_CommandPool {
    VkDevice    device = VK_NULL_HANDLE;
};

// CopyImageToBuffer 延迟回读记录
struct D11_PendingReadback {
    ComPtr<ID3D11Texture2D>     stagingTex;
    VkBuffer                    dstBuffer = VK_NULL_HANDLE;
    uint32_t                    width = 0;
    uint32_t                    height = 0;
    uint32_t                    texelSize = 0;
    VkDeviceSize                bufferOffset = 0;
};

struct D11_CommandBuffer {
    ComPtr<ID3D11DeviceContext>  deferredContext;
    ComPtr<ID3D11CommandList>   commandList;  // FinishCommandList 后的结果
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
    ComPtr<ID3D11Buffer>        pushConstantBuffer;
    uint8_t                     pushConstantData[128] = {};
    uint32_t                    pushConstantSize = 0;
    // 延迟回读列表 (CopyImageToBuffer 在 deferred context 无法直接 Map)
    std::vector<D11_PendingReadback> pendingReadbacks;
};

// ============================================================================
// Render Pass / Framebuffer
// ============================================================================

struct D11_RenderPass {
    std::vector<VkAttachmentDescription> attachments;
    bool                        hasDepth = false;
    uint32_t                    colorAttachmentCount = 0;
    VkFormat                    depthFormat = VK_FORMAT_UNDEFINED;
};

struct D11_Framebuffer {
    std::vector<VkImageView>    attachments;
    uint32_t                    width = 0;
    uint32_t                    height = 0;
    VkRenderPass                renderPass = VK_NULL_HANDLE;
};

// ============================================================================
// Pipeline
// ============================================================================

struct D11_ShaderModule {
    std::vector<char>           bytecode;
    bool                        isImguiReplacement = false;
};

struct D11_PipelineLayout {
    std::vector<VkDescriptorSetLayout>  setLayouts;
    std::vector<VkPushConstantRange>    pushConstantRanges;
    uint32_t                            pushConstSize = 0;
};

struct D11_Pipeline {
    ComPtr<ID3D11VertexShader>      vertexShader;
    ComPtr<ID3D11PixelShader>       pixelShader;
    ComPtr<ID3D11InputLayout>       inputLayout;
    ComPtr<ID3D11RasterizerState>   rasterizerState;
    ComPtr<ID3D11DepthStencilState> depthStencilState;
    ComPtr<ID3D11BlendState>        blendState;
    VkPipelineLayout                layout = VK_NULL_HANDLE;
    D3D11_PRIMITIVE_TOPOLOGY        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    uint32_t                        vertexStrides[8] = {};
    uint32_t                        vertexBindingCount = 0;
    float                           blendFactor[4] = {1, 1, 1, 1};
    uint32_t                        stencilRef = 0;
    std::vector<char>               vsBytecode; // InputLayout 创建需要
    // DXBC RDEF 中实际的 SRV/Sampler register 索引 (按声明顺序排序)
    // flushGraphicsState 绑定第 K 个纹理时使用 srvRegisters[K]
    std::vector<uint32_t>           srvRegisters;
    std::vector<uint32_t>           samplerRegisters;
};

// ============================================================================
// Descriptor
// ============================================================================

struct D11_DescriptorSetLayout {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

struct D11_DescriptorPool {
    VkDevice                        device = VK_NULL_HANDLE;
};

struct D11_DescriptorSet {
    VkDescriptorSetLayout           layout = VK_NULL_HANDLE;
    // UBO 绑定
    ID3D11Buffer*                   cbvBuffers[8] = {};
    VkBuffer                        cbvVkBuffers[8] = {};  // 对应的 VkBuffer (用于 shadow buffer 同步)
    uint32_t                        cbvCount = 0;
    // SRV 绑定
    ID3D11ShaderResourceView*       srvs[8] = {};
    ID3D11SamplerState*             samplers[8] = {};
    uint32_t                        srvCount = 0;
};

// ============================================================================
// Synchronization
// ============================================================================

struct D11_Fence {
    bool                    signaled = false;
};

struct D11_Semaphore {
    // D3D11 immediate mode 无需 semaphore
};

// ============================================================================
// Debug
// ============================================================================

struct D11_DebugMessenger {
    PFN_vkDebugUtilsMessengerCallbackEXT    callback = nullptr;
    void*                                   userData = nullptr;
    VkDebugUtilsMessageSeverityFlagsEXT     severityFilter = 0;
    VkDebugUtilsMessageTypeFlagsEXT         typeFilter = 0;
};

#endif // _WIN32
