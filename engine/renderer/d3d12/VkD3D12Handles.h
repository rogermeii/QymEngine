#pragma once

// ============================================================================
// D3D12 后端 - Vulkan 句柄内部结构体定义
//
// 每个 VkXxx 句柄 (如 VkDevice) 实际是 VkXxx_T* 指针。
// vulkan.h 只做前向声明 (struct VkXxx_T)，此处提供完整定义，
// 内部包含 D3D12 对象。引擎代码无需知道这些细节。
// ============================================================================

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>
#include <dxgi1_6.h>
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

struct VkInstance_T {
    ComPtr<IDXGIFactory4>   factory;
    bool                    debugEnabled = false;
    // 枚举到的物理设备列表 (vkEnumeratePhysicalDevices 时填充)
    std::vector<VkPhysicalDevice> physicalDevices;
};

struct VkPhysicalDevice_T {
    ComPtr<IDXGIAdapter1>   adapter;
    DXGI_ADAPTER_DESC1      adapterDesc = {};
    VkInstance              instance = VK_NULL_HANDLE;
};

struct VkDevice_T {
    ComPtr<ID3D12Device>        device;
    VkPhysicalDevice            physicalDevice = VK_NULL_HANDLE;
    ComPtr<ID3D12CommandQueue>  graphicsQueue;
    ComPtr<ID3D12InfoQueue>     infoQueue;  // D3D12 validation 消息队列
    // 设备级描述符递增大小 (缓存)
    uint32_t rtvDescriptorSize = 0;
    uint32_t dsvDescriptorSize = 0;
    uint32_t cbvSrvUavDescriptorSize = 0;
    uint32_t samplerDescriptorSize = 0;
    // CPU-only 描述符堆 (用于 ImageView 的 RTV/DSV/SRV 分配)
    ComPtr<ID3D12DescriptorHeap>  stagingRtvHeap;
    uint32_t                      stagingRtvNext = 0;
    static constexpr uint32_t     STAGING_RTV_MAX = 128;
    ComPtr<ID3D12DescriptorHeap>  stagingDsvHeap;
    uint32_t                      stagingDsvNext = 0;
    static constexpr uint32_t     STAGING_DSV_MAX = 32;
    ComPtr<ID3D12DescriptorHeap>  stagingSrvHeap;
    uint32_t                      stagingSrvNext = 0;
    static constexpr uint32_t     STAGING_SRV_MAX = 2048;

    D3D12_CPU_DESCRIPTOR_HANDLE allocRtv() {
        auto h = stagingRtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += stagingRtvNext * rtvDescriptorSize;
        stagingRtvNext++;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE allocDsv() {
        auto h = stagingDsvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += stagingDsvNext * dsvDescriptorSize;
        stagingDsvNext++;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE allocSrv() {
        auto h = stagingSrvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += stagingSrvNext * cbvSrvUavDescriptorSize;
        stagingSrvNext++;
        return h;
    }
};

struct VkQueue_T {
    ComPtr<ID3D12CommandQueue>  queue;
    VkDevice                    device = VK_NULL_HANDLE;
    uint32_t                    familyIndex = 0;
    uint32_t                    queueIndex = 0;
};

// ============================================================================
// Surface / Swapchain
// ============================================================================

struct VkSurfaceKHR_T {
    HWND        hwnd = nullptr;
    HINSTANCE   hinstance = nullptr;
};

struct VkSwapchainKHR_T {
    ComPtr<IDXGISwapChain3>             swapChain;
    std::vector<ComPtr<ID3D12Resource>> images;
    // 为每个 image 创建的 VkImage 句柄
    std::vector<VkImage>                imageHandles;
    DXGI_FORMAT                         format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t                            width = 0;
    uint32_t                            height = 0;
    uint32_t                            imageCount = 0;
    uint32_t                            currentIndex = 0;
    // RTV heap (每个 back buffer 一个 RTV)
    ComPtr<ID3D12DescriptorHeap>        rtvHeap;
    uint32_t                            rtvDescriptorSize = 0;
};

// ============================================================================
// Memory / Buffer / Image
// ============================================================================

struct VkDeviceMemory_T {
    ComPtr<ID3D12Resource>  resource;    // committed resource
    D3D12_HEAP_TYPE         heapType = D3D12_HEAP_TYPE_DEFAULT;
    VkDeviceSize            size = 0;
    void*                   mapped = nullptr;
    bool                    isImageMemory = false;  // 区分 buffer/image
};

struct VkBuffer_T {
    ComPtr<ID3D12Resource>  resource;
    VkDeviceSize            size = 0;
    VkBufferUsageFlags      usage = 0;
    D3D12_HEAP_TYPE         heapType = D3D12_HEAP_TYPE_DEFAULT;
    void*                   mapped = nullptr;
    VkDeviceMemory          boundMemory = VK_NULL_HANDLE;
};

struct VkImage_T {
    ComPtr<ID3D12Resource>      resource;
    D3D12_RESOURCE_DESC         resourceDesc = {};
    VkFormat                    format = VK_FORMAT_UNDEFINED;
    VkImageLayout               currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    D3D12_RESOURCE_STATES       currentState = D3D12_RESOURCE_STATE_COMMON;
    uint32_t                    width = 0;
    uint32_t                    height = 0;
    uint32_t                    mipLevels = 1;
    VkImageUsageFlags           usage = 0;
    bool                        ownsResource = true;  // false = swapchain image
    VkDeviceMemory              boundMemory = VK_NULL_HANDLE;
};

struct VkImageView_T {
    VkImage                     image = VK_NULL_HANDLE;
    VkFormat                    format = VK_FORMAT_UNDEFINED;
    VkImageViewType             viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkImageSubresourceRange     subresourceRange = {};
    // D3D12 描述符 (由 DescriptorHeap 分配)
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};     // 用于 RTV
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};     // 用于 DSV
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = {};     // 用于 SRV
    bool                        hasRtv = false;
    bool                        hasDsv = false;
    bool                        hasSrv = false;
};

// ============================================================================
// Sampler
// ============================================================================

struct VkSampler_T {
    D3D12_SAMPLER_DESC          desc = {};
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
};

// ============================================================================
// Command Pool / Command Buffer
// ============================================================================

struct VkCommandPool_T {
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    uint32_t                    nextAllocator = 0;
    D3D12_COMMAND_LIST_TYPE     type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    VkDevice                    device = VK_NULL_HANDLE;
};

struct VkCommandBuffer_T {
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12CommandAllocator>    allocator;
    VkDevice                    device = VK_NULL_HANDLE;
    bool                        isRecording = false;
    // 当前绑定状态 (render pass)
    VkRenderPass                currentRenderPass = VK_NULL_HANDLE;
    VkFramebuffer               currentFramebuffer = VK_NULL_HANDLE;
    // 延迟绑定状态 (在 draw 时才真正提交到 D3D12)
    VkPipeline                  currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet             boundSets[4] = {};
    VkPipelineLayout            boundLayout = VK_NULL_HANDLE;  // 最后一次 bind 使用的 layout
    VkDescriptorPool            boundPool = VK_NULL_HANDLE;    // descriptor pool (用于 heap 设置)
    bool                        stateDirty = false;            // 状态变化标记
    uint32_t                    srvScratchOffset = 0;          // 每帧 SRV scratch 分配偏移
};

// ============================================================================
// Render Pass / Framebuffer
// ============================================================================

struct VkRenderPass_T {
    std::vector<VkAttachmentDescription> attachments;
    // 简化: 只存 color 和 depth 格式信息
    bool                        hasDepth = false;
    uint32_t                    colorAttachmentCount = 0;
    VkFormat                    depthFormat = VK_FORMAT_UNDEFINED;
};

struct VkFramebuffer_T {
    std::vector<VkImageView>    attachments;
    uint32_t                    width = 0;
    uint32_t                    height = 0;
    VkRenderPass                renderPass = VK_NULL_HANDLE;
};

// ============================================================================
// Pipeline
// ============================================================================

struct VkShaderModule_T {
    std::vector<char>           bytecode;   // DXIL 字节码
    bool                        isImguiReplacement = false; // true = SPIR-V 已替换为 ImGui DXIL
};

struct VkPipelineLayout_T {
    ComPtr<ID3D12RootSignature>     rootSignature;
    std::vector<VkDescriptorSetLayout> setLayouts;
    std::vector<VkPushConstantRange>   pushConstantRanges;
    uint32_t                        rootParamCount = 0;
    uint32_t                        srvTableRootIdx = UINT32_MAX;
    uint32_t                        pushConstRootIdx = UINT32_MAX; // root constants 的 root param index
};

struct VkPipeline_T {
    ComPtr<ID3D12PipelineState>     pipelineState;
    VkPipelineLayout                layout = VK_NULL_HANDLE;
    D3D12_PRIMITIVE_TOPOLOGY        topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    // Vertex input strides (per binding slot)
    uint32_t                        vertexStrides[8] = {};
    uint32_t                        vertexBindingCount = 0;
};

// ============================================================================
// Descriptor
// ============================================================================

struct VkDescriptorSetLayout_T {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

struct VkDescriptorPool_T {
    ComPtr<ID3D12DescriptorHeap>    cbvSrvUavHeap;
    ComPtr<ID3D12DescriptorHeap>    samplerHeap;
    uint32_t                        maxCbvSrvUav = 0;
    uint32_t                        maxSamplers = 0;
    uint32_t                        allocatedCbvSrvUav = 0;
    uint32_t                        allocatedSamplers = 0;
    VkDevice                        device = VK_NULL_HANDLE;
    // 每帧 SRV 合并用的临时区域 (从 heap 尾部分配，每帧重置)
    uint32_t                        srvScratchBase = 0;   // 临时区域起始
    uint32_t                        srvScratchNext = 0;   // 当前分配位置
};

struct VkDescriptorSet_T {
    D3D12_GPU_DESCRIPTOR_HANDLE     gpuHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE     cpuHandle = {};
    uint32_t                        descriptorCount = 0;
    VkDescriptorPool                pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout           layout = VK_NULL_HANDLE;
    // Root CBV 地址 (per binding, 用于 SetGraphicsRootConstantBufferView)
    D3D12_GPU_VIRTUAL_ADDRESS       cbvAddresses[8] = {};
    uint32_t                        cbvCount = 0;
};

// ============================================================================
// Synchronization
// ============================================================================

struct VkFence_T {
    ComPtr<ID3D12Fence>     fence;
    HANDLE                  event = nullptr;
    uint64_t                value = 0;
    bool                    signaled = false;
};

struct VkSemaphore_T {
    ComPtr<ID3D12Fence>     fence;
    uint64_t                value = 0;
};

// ============================================================================
// Debug
// ============================================================================

struct VkDebugUtilsMessengerEXT_T {
    PFN_vkDebugUtilsMessengerCallbackEXT    callback = nullptr;
    void*                                   userData = nullptr;
    VkDebugUtilsMessageSeverityFlagsEXT     severityFilter = 0;
    VkDebugUtilsMessageTypeFlagsEXT         typeFilter = 0;
};

#endif // _WIN32
