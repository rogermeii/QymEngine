#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "renderer/d3d12/VkD3D12.h"
#include "renderer/d3d12/VkD3D12Handles.h"
#include "renderer/VkDispatch.h"
#include <iostream>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// ============================================================================
// 辅助工具
// ============================================================================

// ImGui DXIL 着色器 (从 imgui_vs.hlsl / imgui_ps.hlsl 编译)
#include "renderer/d3d12/imgui_vs_dxil.h"
#include "renderer/d3d12/imgui_ps_dxil.h"

#define D3D12_STUB(fn) \
    do { std::cerr << "[D3D12 Stub] " << #fn << " not implemented" << std::endl; } while(0)

// 延迟释放列表 (临时 buffer 需要等 GPU 执行完才能释放)
static std::vector<ComPtr<ID3D12Resource>> s_deferredRelease;
static void deferRelease(ComPtr<ID3D12Resource> res) { s_deferredRelease.push_back(std::move(res)); }
static void flushDeferredRelease() { s_deferredRelease.clear(); }

// 刷新 D3D12 InfoQueue 中的 validation 消息
static void flushD3D12Messages(VkDevice device)
{
    if (!device || !device->infoQueue) return;
    UINT64 count = device->infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; i++) {
        SIZE_T msgLen = 0;
        device->infoQueue->GetMessage(i, nullptr, &msgLen);
        if (msgLen == 0) continue;
        auto* msg = (D3D12_MESSAGE*)malloc(msgLen);
        if (!msg) continue;
        device->infoQueue->GetMessage(i, msg, &msgLen);
        const char* severity = "INFO";
        if (msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR)       severity = "ERROR";
        else if (msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING) severity = "WARNING";
        else if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) severity = "CORRUPTION";
        std::cerr << "[D3D12 " << severity << "] " << msg->pDescription << std::endl;
        free(msg);
    }
    device->infoQueue->ClearStoredMessages();
}

// VkFormat → DXGI_FORMAT 转换 (常用格式)
static DXGI_FORMAT vkFormatToDxgi(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:             return DXGI_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_UNORM:           return DXGI_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:       return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:       return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VK_FORMAT_R32G32B32A32_SFLOAT:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R32G32B32_SFLOAT:     return DXGI_FORMAT_R32G32B32_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT:        return DXGI_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32_SFLOAT:           return DXGI_FORMAT_R32_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_D32_SFLOAT:           return DXGI_FORMAT_D32_FLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case VK_FORMAT_D16_UNORM:            return DXGI_FORMAT_D16_UNORM;
    default:                             return DXGI_FORMAT_UNKNOWN;
    }
}

// D3D12 heap type 从 Vulkan memoryTypeIndex 推导
// 我们的内存类型映射:
//   0 = DEVICE_LOCAL (D3D12_HEAP_TYPE_DEFAULT)
//   1 = HOST_VISIBLE | HOST_COHERENT (D3D12_HEAP_TYPE_UPLOAD)
//   2 = HOST_VISIBLE | HOST_COHERENT | HOST_CACHED (D3D12_HEAP_TYPE_READBACK)
static D3D12_HEAP_TYPE memoryTypeToHeapType(uint32_t memoryTypeIndex)
{
    switch (memoryTypeIndex) {
    case 0:  return D3D12_HEAP_TYPE_DEFAULT;
    case 1:  return D3D12_HEAP_TYPE_UPLOAD;
    case 2:  return D3D12_HEAP_TYPE_READBACK;
    default: return D3D12_HEAP_TYPE_DEFAULT;
    }
}

// ============================================================================
// Instance
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    (void)pCreateInfo; (void)pAllocator;

    auto* inst = new VkInstance_T();

    UINT dxgiFlags = 0;
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
            inst->debugEnabled = true;
            std::cout << "[VkD3D12] Debug layer enabled" << std::endl;
        }
    }
#endif

    if (FAILED(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&inst->factory)))) {
        delete inst;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    *pInstance = inst;
    std::cout << "[VkD3D12] Instance created" << std::endl;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator)
{
    (void)pAllocator;
    if (!instance) return;

    // 释放枚举的物理设备
    for (auto pd : instance->physicalDevices) {
        delete pd;
    }
    instance->physicalDevices.clear();

    delete instance;
    std::cout << "[VkD3D12] Instance destroyed" << std::endl;
}

// ============================================================================
// Physical Device
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices)
{
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;

    // 首次调用时枚举 DXGI 适配器
    if (instance->physicalDevices.empty()) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; instance->factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            // 检查是否支持 D3D12
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                            __uuidof(ID3D12Device), nullptr))) {
                auto* pd = new VkPhysicalDevice_T();
                pd->adapter = adapter;
                pd->adapterDesc = desc;
                pd->instance = instance;
                instance->physicalDevices.push_back(pd);
            }
            adapter.Reset();
        }
    }

    uint32_t count = static_cast<uint32_t>(instance->physicalDevices.size());
    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = count;
        return VK_SUCCESS;
    }

    uint32_t toWrite = std::min(*pPhysicalDeviceCount, count);
    for (uint32_t i = 0; i < toWrite; i++) {
        pPhysicalDevices[i] = instance->physicalDevices[i];
    }
    *pPhysicalDeviceCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties)
{
    if (!physicalDevice || !pProperties) return;
    memset(pProperties, 0, sizeof(*pProperties));

    pProperties->apiVersion = VK_API_VERSION_1_0;
    pProperties->driverVersion = 1;
    pProperties->vendorID = physicalDevice->adapterDesc.VendorId;
    pProperties->deviceID = physicalDevice->adapterDesc.DeviceId;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

    // 适配器名称 (UTF-16 → UTF-8)
    WideCharToMultiByte(CP_UTF8, 0, physicalDevice->adapterDesc.Description, -1,
                        pProperties->deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE,
                        nullptr, nullptr);

    // limits
    pProperties->limits.maxImageDimension2D = 16384;
    pProperties->limits.maxPushConstantsSize = 256;
    pProperties->limits.maxBoundDescriptorSets = 4;
    pProperties->limits.maxPerStageDescriptorSampledImages = 1000000;
    pProperties->limits.maxPerStageDescriptorSamplers = 2048;
    pProperties->limits.minUniformBufferOffsetAlignment = 256;
    pProperties->limits.nonCoherentAtomSize = 64;  // D3D12 无此概念，但 ImGui 需要非零值
}

static void VKAPI_CALL d3d12_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures)
{
    (void)physicalDevice;
    if (!pFeatures) return;
    memset(pFeatures, 0, sizeof(*pFeatures));

    pFeatures->samplerAnisotropy = VK_TRUE;
    pFeatures->fillModeNonSolid = VK_TRUE;
    pFeatures->wideLines = VK_FALSE;
    pFeatures->multiDrawIndirect = VK_TRUE;
}

static void VKAPI_CALL d3d12_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures)
{
    if (!pFeatures) return;
    d3d12_vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);

    // 遍历 pNext 链，填充扩展特性
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(pFeatures->pNext);
    while (next) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            auto* indexing = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(next);
            // D3D12 天然支持 bindless
            indexing->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            indexing->runtimeDescriptorArray = VK_TRUE;
            indexing->descriptorBindingPartiallyBound = VK_TRUE;
            indexing->descriptorBindingVariableDescriptorCount = VK_TRUE;
            indexing->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            indexing->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        }
        next = next->pNext;
    }
}

static void VKAPI_CALL d3d12_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties)
{
    (void)physicalDevice;
    if (!pMemoryProperties) return;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));

    // 1 个 memory heap (全部显存)
    pMemoryProperties->memoryHeapCount = 1;
    pMemoryProperties->memoryHeaps[0].size = 4ULL * 1024 * 1024 * 1024; // 4GB
    pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

    // 3 种 memory type
    pMemoryProperties->memoryTypeCount = 3;

    // Type 0: DEVICE_LOCAL (D3D12_HEAP_TYPE_DEFAULT)
    pMemoryProperties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;

    // Type 1: HOST_VISIBLE | HOST_COHERENT (D3D12_HEAP_TYPE_UPLOAD)
    pMemoryProperties->memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMemoryProperties->memoryTypes[1].heapIndex = 0;

    // Type 2: HOST_VISIBLE | HOST_COHERENT | HOST_CACHED (D3D12_HEAP_TYPE_READBACK)
    pMemoryProperties->memoryTypes[2].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    pMemoryProperties->memoryTypes[2].heapIndex = 0;
}

static void VKAPI_CALL d3d12_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice,
    uint32_t* pQueueFamilyPropertyCount,
    VkQueueFamilyProperties* pQueueFamilyProperties)
{
    (void)physicalDevice;
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }

    uint32_t toWrite = std::min(*pQueueFamilyPropertyCount, 1u);
    if (toWrite > 0) {
        pQueueFamilyProperties[0] = {};
        pQueueFamilyProperties[0].queueFlags =
            VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        pQueueFamilyProperties[0].queueCount = 1;
        pQueueFamilyProperties[0].timestampValidBits = 64;
        pQueueFamilyProperties[0].minImageTransferGranularity = {1, 1, 1};
    }
    *pQueueFamilyPropertyCount = toWrite;
}

static VkResult VKAPI_CALL d3d12_vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    (void)pLayerName; (void)pProperties;
    // 报告支持 surface + win32_surface 扩展
    const VkExtensionProperties exts[] = {
        {"VK_KHR_surface", 1},
        {"VK_KHR_win32_surface", 1},
    };
    uint32_t count = 2;

    if (!pProperties) {
        *pPropertyCount = count;
        return VK_SUCCESS;
    }
    uint32_t toWrite = std::min(*pPropertyCount, count);
    memcpy(pProperties, exts, toWrite * sizeof(VkExtensionProperties));
    *pPropertyCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount,
    VkLayerProperties* pProperties)
{
    (void)pProperties;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    (void)physicalDevice; (void)pLayerName;
    const VkExtensionProperties exts[] = {
        {"VK_KHR_swapchain", 1},
    };
    uint32_t count = 1;

    if (!pProperties) {
        *pPropertyCount = count;
        return VK_SUCCESS;
    }
    uint32_t toWrite = std::min(*pPropertyCount, count);
    memcpy(pProperties, exts, toWrite * sizeof(VkExtensionProperties));
    *pPropertyCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

// ============================================================================
// Device & Queue
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    (void)pCreateInfo; (void)pAllocator;
    if (!physicalDevice) return VK_ERROR_INITIALIZATION_FAILED;

    auto* dev = new VkDevice_T();
    dev->physicalDevice = physicalDevice;

    HRESULT hr = D3D12CreateDevice(physicalDevice->adapter.Get(),
                                   D3D_FEATURE_LEVEL_12_0,
                                   IID_PPV_ARGS(&dev->device));
    if (FAILED(hr)) {
        delete dev;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // 创建图形命令队列
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dev->graphicsQueue));

    // 缓存描述符大小
    dev->rtvDescriptorSize = dev->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dev->dsvDescriptorSize = dev->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    dev->cbvSrvUavDescriptorSize = dev->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dev->samplerDescriptorSize = dev->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // 创建 CPU-only staging 描述符堆 (用于 ImageView)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = VkDevice_T::STAGING_RTV_MAX;
        dev->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dev->stagingRtvHeap));

        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = VkDevice_T::STAGING_DSV_MAX;
        dev->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dev->stagingDsvHeap));

        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = VkDevice_T::STAGING_SRV_MAX;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only, 不是 shader-visible
        dev->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dev->stagingSrvHeap));
    }

    // 配置 D3D12 InfoQueue (validation 消息)
    if (SUCCEEDED(dev->device.As(&dev->infoQueue))) {
        // 暂不设置 break-on-error，只记录消息
        // dev->infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        // dev->infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);

        // 过滤掉一些无害的常见警告
        D3D12_MESSAGE_ID denyIds[] = {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
        };
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = _countof(denyIds);
        filter.DenyList.pIDList = denyIds;
        dev->infoQueue->PushStorageFilter(&filter);
        std::cout << "[VkD3D12] InfoQueue configured (validation enabled)" << std::endl;
    }

    *pDevice = dev;

    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, physicalDevice->adapterDesc.Description, -1,
                        name, 256, nullptr, nullptr);
    std::cout << "[VkD3D12] Device created on: " << name << std::endl;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator)
{
    (void)pAllocator;
    if (!device) return;
    delete device;
    std::cout << "[VkD3D12] Device destroyed" << std::endl;
}

static void VKAPI_CALL d3d12_vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue)
{
    if (!device || !pQueue) return;

    // 所有 queue family 都返回同一个图形队列
    auto* q = new VkQueue_T();
    q->queue = device->graphicsQueue;
    q->device = device;
    q->familyIndex = queueFamilyIndex;
    q->queueIndex = queueIndex;
    *pQueue = q;
}

static VkResult VKAPI_CALL d3d12_vkDeviceWaitIdle(VkDevice device)
{
    if (!device) return VK_ERROR_DEVICE_LOST;

    // 创建临时 fence 等待 GPU 完成
    ComPtr<ID3D12Fence> fence;
    device->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    device->graphicsQueue->Signal(fence.Get(), 1);

    if (fence->GetCompletedValue() < 1) {
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
    }
    return VK_SUCCESS;
}

// ============================================================================
// Surface (Win32)
// ============================================================================

#ifdef VK_USE_PLATFORM_WIN32_KHR
static VkResult VKAPI_CALL d3d12_vkCreateWin32SurfaceKHR(
    VkInstance instance,
    const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface)
{
    (void)instance; (void)pAllocator;
    auto* surface = new VkSurfaceKHR_T();
    surface->hwnd = pCreateInfo->hwnd;
    surface->hinstance = pCreateInfo->hinstance;
    *pSurface = surface;
    return VK_SUCCESS;
}
#endif

static void VKAPI_CALL d3d12_vkDestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator)
{
    (void)instance; (void)pAllocator;
    delete surface;
}

static VkResult VKAPI_CALL d3d12_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
    (void)physicalDevice;
    if (!surface || !pSurfaceCapabilities) return VK_ERROR_SURFACE_LOST_KHR;

    RECT rect;
    GetClientRect(surface->hwnd, &rect);
    uint32_t w = rect.right - rect.left;
    uint32_t h = rect.bottom - rect.top;

    pSurfaceCapabilities->minImageCount = 2;
    pSurfaceCapabilities->maxImageCount = 3;
    pSurfaceCapabilities->currentExtent = {w, h};
    pSurfaceCapabilities->minImageExtent = {w, h};
    pSurfaceCapabilities->maxImageExtent = {w, h};
    pSurfaceCapabilities->maxImageArrayLayers = 1;
    pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    pSurfaceCapabilities->supportedUsageFlags =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats)
{
    (void)physicalDevice; (void)surface;
    const VkSurfaceFormatKHR formats[] = {
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    uint32_t count = 3;
    if (!pSurfaceFormats) { *pSurfaceFormatCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pSurfaceFormatCount, count);
    memcpy(pSurfaceFormats, formats, toWrite * sizeof(VkSurfaceFormatKHR));
    *pSurfaceFormatCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes)
{
    (void)physicalDevice; (void)surface;
    const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    uint32_t count = 2;
    if (!pPresentModes) { *pPresentModeCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pPresentModeCount, count);
    memcpy(pPresentModes, modes, toWrite * sizeof(VkPresentModeKHR));
    *pPresentModeCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported)
{
    (void)physicalDevice; (void)queueFamilyIndex; (void)surface;
    *pSupported = VK_TRUE;
    return VK_SUCCESS;
}

// ============================================================================
// Memory
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkAllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory)
{
    (void)pAllocator;
    if (!device || !pAllocateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* mem = new VkDeviceMemory_T();
    mem->heapType = memoryTypeToHeapType(pAllocateInfo->memoryTypeIndex);
    mem->size = pAllocateInfo->allocationSize;
    // resource 在 vkBindBufferMemory / vkBindImageMemory 时创建 (committed resource)
    *pMemory = mem;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!memory) return;
    if (memory->mapped && memory->resource) {
        memory->resource->Unmap(0, nullptr);
        memory->mapped = nullptr;
    }
    // 延迟释放 resource (确保 GPU 完成使用)
    if (memory->resource) {
        deferRelease(std::move(memory->resource));
    }
    delete memory;
}

static VkResult VKAPI_CALL d3d12_vkMapMemory(
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData)
{
    (void)device; (void)size; (void)flags;
    if (!memory || !memory->resource) {
        fprintf(stderr, "[VkD3D12] MapMemory FAILED: memory=%p resource=%p\n",
                (void*)memory, memory ? (void*)memory->resource.Get() : nullptr);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    if (!memory->mapped) {
        HRESULT hr = memory->resource->Map(0, nullptr, &memory->mapped);
        if (FAILED(hr)) {
            fprintf(stderr, "[VkD3D12] MapMemory Map() FAILED: hr=0x%lx heapType=%d\n", hr, (int)memory->heapType);
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
    }
    *ppData = static_cast<uint8_t*>(memory->mapped) + offset;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkUnmapMemory(
    VkDevice device,
    VkDeviceMemory memory)
{
    (void)device;
    if (!memory || !memory->resource) return;
    // UPLOAD heap 保持映射状态（UBO 需要持久映射，每帧 memcpy 更新）
    // READBACK heap 需要 unmap 才能让 GPU 写入
    if (memory->heapType == D3D12_HEAP_TYPE_READBACK) {
        memory->resource->Unmap(0, nullptr);
        memory->mapped = nullptr;
    }
    // 注意: 对于 staging buffer, 虽然不 unmap, 但 CopyBuffer 中的
    // Map/memcpy/Unmap 会正确刷新 write-combine buffer
}

static int s_mapCount = 0;
static VkResult VKAPI_CALL d3d12_vkFlushMappedMemoryRanges(
    VkDevice device,
    uint32_t memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges)
{
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    // D3D12 upload heap 是 write-combined，不需要显式 flush
    return VK_SUCCESS;
}

// ============================================================================
// Buffer
// ============================================================================

static int s_createBufCount = 0;
static VkResult VKAPI_CALL d3d12_vkCreateBuffer(
    VkDevice device,
    const VkBufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkBuffer* pBuffer)
{
    (void)pAllocator;
    if (!device || !pCreateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* buf = new VkBuffer_T();
    buf->size = pCreateInfo->size;
    buf->usage = pCreateInfo->usage;
    fprintf(stderr, "[VkD3D12] CreateBuffer #%d size=%llu usage=0x%x\n", s_createBufCount, (unsigned long long)pCreateInfo->size, pCreateInfo->usage);
    s_createBufCount++;
    *pBuffer = buf;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyBuffer(
    VkDevice device,
    VkBuffer buffer,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!buffer) return;
    delete buffer;
}

static void VKAPI_CALL d3d12_vkGetBufferMemoryRequirements(
    VkDevice device,
    VkBuffer buffer,
    VkMemoryRequirements* pMemoryRequirements)
{
    (void)device;
    if (!buffer || !pMemoryRequirements) return;

    pMemoryRequirements->size = (buffer->size + 255) & ~255ULL; // D3D12 要求 256 对齐
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0x7; // type 0, 1, 2
}

static VkResult VKAPI_CALL d3d12_vkBindBufferMemory(
    VkDevice device,
    VkBuffer buffer,
    VkDeviceMemory memory,
    VkDeviceSize memoryOffset)
{
    (void)memoryOffset;
    if (!device || !buffer || !memory) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    buffer->boundMemory = memory;

    // D3D12 heap 选择:
    // - TRANSFER_DST (readback) → 用 READBACK heap (GPU 写 → CPU 读)
    // - 其他 DEFAULT → 用 UPLOAD heap (CPU 写 → GPU 读，简化 staging copy)
    D3D12_HEAP_TYPE heapType = memory->heapType;
    if (heapType == D3D12_HEAP_TYPE_DEFAULT) {
        if (buffer->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT &&
            !(buffer->usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))) {
            // 纯 transfer dst (readback 用途) → READBACK heap
            heapType = D3D12_HEAP_TYPE_READBACK;
        } else {
            heapType = D3D12_HEAP_TYPE_UPLOAD;
        }
    }
    buffer->heapType = heapType;

    // 创建 committed resource
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heapType;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // D3D12 不接受 0 大小的 buffer
    VkDeviceSize bufSize = (buffer->size > 0) ? buffer->size : 256;
    rd.Width = (bufSize + 255) & ~255ULL;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_RESOURCE_STATES initialState;
    if (heapType == D3D12_HEAP_TYPE_UPLOAD)
        initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heapType == D3D12_HEAP_TYPE_READBACK)
        initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    else
        initialState = D3D12_RESOURCE_STATE_COMMON;

    HRESULT hr = device->device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initialState, nullptr,
        IID_PPV_ARGS(&buffer->resource));

    if (FAILED(hr)) {
        fprintf(stderr, "[VkD3D12] BindBufferMemory: CreateCommittedResource FAILED hr=0x%lx size=%llu heapType=%d\n",
                hr, (unsigned long long)rd.Width, (int)memory->heapType);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    // 同步 memory 的 resource 引用
    memory->resource = buffer->resource;
    return VK_SUCCESS;
}

// ============================================================================
// Image
// ============================================================================

static int s_createImgCount = 0;
static VkResult VKAPI_CALL d3d12_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage)
{
    (void)pAllocator;
    if (!device || !pCreateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    fprintf(stderr, "[VkD3D12] CreateImage #%d %dx%d fmt=%d usage=0x%x\n",
            s_createImgCount, pCreateInfo->extent.width, pCreateInfo->extent.height,
            pCreateInfo->format, pCreateInfo->usage);
    s_createImgCount++;

    auto* img = new VkImage_T();
    img->format = pCreateInfo->format;
    img->width = pCreateInfo->extent.width;
    img->height = pCreateInfo->extent.height;
    img->mipLevels = pCreateInfo->mipLevels;
    img->usage = pCreateInfo->usage;
    img->currentLayout = pCreateInfo->initialLayout;

    // 构建 D3D12_RESOURCE_DESC
    img->resourceDesc = {};
    img->resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    img->resourceDesc.Width = img->width;
    img->resourceDesc.Height = img->height;
    img->resourceDesc.DepthOrArraySize = 1;
    img->resourceDesc.MipLevels = static_cast<UINT16>(img->mipLevels);
    img->resourceDesc.Format = vkFormatToDxgi(img->format);
    img->resourceDesc.SampleDesc.Count = 1;

    if (pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        img->resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
        img->resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // resource 在 vkBindImageMemory 时创建
    *pImage = img;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!image) return;
    if (image->ownsResource) {
        delete image;
    }
    // swapchain image 不 delete (由 swapchain 管理)
}

static void VKAPI_CALL d3d12_vkGetImageMemoryRequirements(
    VkDevice device,
    VkImage image,
    VkMemoryRequirements* pMemoryRequirements)
{
    if (!device || !image || !pMemoryRequirements) return;

    // 使用 D3D12 获取精确的资源分配信息
    D3D12_RESOURCE_ALLOCATION_INFO allocInfo =
        device->device->GetResourceAllocationInfo(0, 1, &image->resourceDesc);

    pMemoryRequirements->size = allocInfo.SizeInBytes;
    pMemoryRequirements->alignment = allocInfo.Alignment;
    pMemoryRequirements->memoryTypeBits = 0x1; // 只支持 DEVICE_LOCAL
}

static VkResult VKAPI_CALL d3d12_vkBindImageMemory(
    VkDevice device,
    VkImage image,
    VkDeviceMemory memory,
    VkDeviceSize memoryOffset)
{
    fprintf(stderr, "[VkD3D12] BindImageMemory: fmt=0x%x %ux%u flags=0x%x heapType=%d\n",
            image ? (int)image->resourceDesc.Format : -1,
            image ? (uint32_t)image->resourceDesc.Width : 0,
            image ? image->resourceDesc.Height : 0,
            image ? (int)image->resourceDesc.Flags : -1,
            memory ? (int)memory->heapType : -1);
    (void)memoryOffset;
    if (!device || !image || !memory) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    image->boundMemory = memory;
    memory->isImageMemory = true;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CLEAR_VALUE* clearValue = nullptr;
    D3D12_CLEAR_VALUE cv = {};

    if (image->resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
        initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        image->currentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        cv.Format = image->resourceDesc.Format;
        cv.DepthStencil.Depth = 1.0f;
        clearValue = &cv;
    } else if (image->resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
        initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        image->currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cv.Format = image->resourceDesc.Format;
        clearValue = &cv;
    }

    HRESULT hr = device->device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &image->resourceDesc, initialState,
        clearValue, IID_PPV_ARGS(&image->resource));

    if (FAILED(hr)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    memory->resource = image->resource;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView)
{
    (void)pAllocator;
    if (!device || !pCreateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* view = new VkImageView_T();
    view->image = pCreateInfo->image;
    view->format = pCreateInfo->format;
    view->viewType = pCreateInfo->viewType;
    view->subresourceRange = pCreateInfo->subresourceRange;

    VkImage img = pCreateInfo->image;
    fprintf(stderr, "[VkD3D12] CreateImageView: img=%p res=%p fmt=%d usage=0x%x %dx%d\n",
            (void*)img, img ? (void*)img->resource.Get() : nullptr,
            pCreateInfo->format, img ? img->usage : 0,
            img ? img->width : 0, img ? img->height : 0);
    if (img && img->resource) {
        DXGI_FORMAT dxgiFormat = vkFormatToDxgi(pCreateInfo->format);
        VkImageUsageFlags usage = img->usage;

        // Depth format → 创建 DSV
        bool isDepth = (pCreateInfo->subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
                       (dxgiFormat == DXGI_FORMAT_D32_FLOAT || dxgiFormat == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                        dxgiFormat == DXGI_FORMAT_D16_UNORM);
        if (isDepth) {
            view->dsvHandle = device->allocDsv();
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = dxgiFormat;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device->device->CreateDepthStencilView(img->resource.Get(), &dsvDesc, view->dsvHandle);
            view->hasDsv = true;
        }

        // Color attachment → 创建 RTV
        if (dxgiFormat == DXGI_FORMAT_UNKNOWN && img->resourceDesc.Format != DXGI_FORMAT_UNKNOWN)
            dxgiFormat = img->resourceDesc.Format; // fallback 到 image 自身的格式
        if (!isDepth && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            view->rtvHandle = device->allocRtv();
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = dxgiFormat;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->device->CreateRenderTargetView(img->resource.Get(), &rtvDesc, view->rtvHandle);
            view->hasRtv = true;
        }

        // Sampled/input → 创建 SRV
        if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
            view->srvHandle = device->allocSrv();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            // Depth SRV 需要用 typeless 的 float 格式
            if (isDepth && dxgiFormat == DXGI_FORMAT_D32_FLOAT)
                srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            else if (isDepth && dxgiFormat == DXGI_FORMAT_D24_UNORM_S8_UINT)
                srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            else
                srvDesc.Format = dxgiFormat;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = (img->mipLevels > 0) ? img->mipLevels : 1;
            device->device->CreateShaderResourceView(img->resource.Get(), &srvDesc, view->srvHandle);
            view->hasSrv = true;
        }
    }

    *pView = view;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyImageView(
    VkDevice device,
    VkImageView imageView,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete imageView;
}

// ============================================================================
// Sampler
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* sampler = new VkSampler_T();
    sampler->desc = {};
    sampler->desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; // 简化
    sampler->desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler->desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler->desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler->desc.MaxLOD = D3D12_FLOAT32_MAX;
    *pSampler = sampler;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroySampler(
    VkDevice device,
    VkSampler sampler,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete sampler;
}

// ============================================================================
// Command Pool & Buffer
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateCommandPool(
    VkDevice device,
    const VkCommandPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkCommandPool* pCommandPool)
{
    (void)pAllocator;
    if (!device) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* pool = new VkCommandPool_T();
    pool->type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    pool->device = device;
    *pCommandPool = pool;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete commandPool;
}

static VkResult VKAPI_CALL d3d12_vkResetCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolResetFlags flags)
{
    (void)device; (void)flags;
    if (!commandPool) return VK_ERROR_INITIALIZATION_FAILED;
    // 重置所有 command allocator
    for (auto& alloc : commandPool->allocators) {
        if (alloc) alloc->Reset();
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkAllocateCommandBuffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers)
{
    if (!device || !pAllocateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* pool = pAllocateInfo->commandPool;
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
        auto* cmd = new VkCommandBuffer_T();
        cmd->device = device;

        // 为每个 command buffer 创建一个 allocator
        ComPtr<ID3D12CommandAllocator> allocator;
        device->device->CreateCommandAllocator(pool->type, IID_PPV_ARGS(&allocator));
        cmd->allocator = allocator;
        pool->allocators.push_back(allocator);

        device->device->CreateCommandList(0, pool->type, allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&cmd->commandList));
        cmd->commandList->Close(); // 初始状态为关闭

        pCommandBuffers[i] = cmd;
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkFreeCommandBuffers(
    VkDevice device,
    VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers)
{
    (void)device; (void)commandPool;
    for (uint32_t i = 0; i < commandBufferCount; i++) {
        delete pCommandBuffers[i];
    }
}

static VkResult VKAPI_CALL d3d12_vkBeginCommandBuffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo)
{
    (void)pBeginInfo;
    if (!commandBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    commandBuffer->allocator->Reset();
    commandBuffer->commandList->Reset(commandBuffer->allocator.Get(), nullptr);
    commandBuffer->isRecording = true;
    // 重置延迟绑定状态
    commandBuffer->currentPipeline = VK_NULL_HANDLE;
    memset(commandBuffer->boundSets, 0, sizeof(commandBuffer->boundSets));
    commandBuffer->boundPool = VK_NULL_HANDLE;
    commandBuffer->boundLayout = VK_NULL_HANDLE;
    commandBuffer->stateDirty = false;
    commandBuffer->srvScratchOffset = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    if (!commandBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    // 在 Close 之前先 flush validation 消息，捕获录制过程中的错误
    if (commandBuffer->device) flushD3D12Messages(commandBuffer->device);
    HRESULT hr = commandBuffer->commandList->Close();
    if (FAILED(hr)) {
        fprintf(stderr, "[VkD3D12] EndCommandBuffer: Close FAILED hr=0x%lx\n", hr);
        if (commandBuffer->device) flushD3D12Messages(commandBuffer->device);
    }
    commandBuffer->isRecording = false;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkResetCommandBuffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags)
{
    (void)flags;
    if (!commandBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    // Reset 在 BeginCommandBuffer 中处理
    return VK_SUCCESS;
}

// ============================================================================
// Synchronization
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateFence(
    VkDevice device,
    const VkFenceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFence* pFence)
{
    (void)pAllocator;
    if (!device) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* f = new VkFence_T();
    device->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f->fence));
    f->event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    f->signaled = (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    if (f->signaled) f->value = 1;
    *pFence = f;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyFence(
    VkDevice device,
    VkFence fence,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!fence) return;
    if (fence->event) CloseHandle(fence->event);
    delete fence;
}

static uint32_t s_waitCount = 0;
static VkResult VKAPI_CALL d3d12_vkWaitForFences(
    VkDevice device,
    uint32_t fenceCount,
    const VkFence* pFences,
    VkBool32 waitAll,
    uint64_t timeout)
{
    (void)device; (void)waitAll;
    for (uint32_t i = 0; i < fenceCount; i++) {
        auto* f = pFences[i];
        if (!f) continue;
        if (f->signaled) {
            if (s_waitCount < 5) std::cout << "[VkD3D12] WaitForFences #" << s_waitCount << " already signaled" << std::endl;
            s_waitCount++;
            continue;
        }
        if (s_waitCount < 5) std::cout << "[VkD3D12] WaitForFences #" << s_waitCount << " waiting (completed=" << f->fence->GetCompletedValue() << " need=" << f->value << ")" << std::endl;
        if (f->fence->GetCompletedValue() < f->value) {
            f->fence->SetEventOnCompletion(f->value, f->event);
            DWORD waitMs = (timeout == UINT64_MAX) ? INFINITE : (DWORD)(timeout / 1000000);
            WaitForSingleObject(f->event, waitMs);
        }
        if (s_waitCount < 5) std::cout << "[VkD3D12] WaitForFences #" << s_waitCount << " done" << std::endl;
        f->signaled = true;
        s_waitCount++;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkResetFences(
    VkDevice device,
    uint32_t fenceCount,
    const VkFence* pFences)
{
    (void)device;
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (pFences[i]) pFences[i]->signaled = false;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence)
{
    if (!queue) return VK_ERROR_DEVICE_LOST;

    for (uint32_t i = 0; i < submitCount; i++) {
        for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
            auto* cmd = pSubmits[i].pCommandBuffers[j];
            ID3D12CommandList* lists[] = {cmd->commandList.Get()};
            queue->queue->ExecuteCommandLists(1, lists);
        }
    }

    if (fence) {
        fence->value++;
        queue->queue->Signal(fence->fence.Get(), fence->value);
    }

    // 刷新 validation 消息
    if (queue->device) flushD3D12Messages(queue->device);

    static uint32_t s_submitCount = 0;
    if (s_submitCount < 20) {
        fprintf(stderr, "[VkD3D12] QueueSubmit #%d (cmds=%d, fence=%s)\n",
                s_submitCount,
                (submitCount > 0 ? pSubmits[0].commandBufferCount : 0),
                (fence ? "yes" : "no"));
    }
    s_submitCount++;

    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkQueueWaitIdle(VkQueue queue)
{
    if (!queue || !queue->device) return VK_ERROR_DEVICE_LOST;
    auto result = d3d12_vkDeviceWaitIdle(queue->device);
    flushDeferredRelease();
    return result;
}

static VkResult VKAPI_CALL d3d12_vkCreateSemaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSemaphore* pSemaphore)
{
    (void)pCreateInfo; (void)pAllocator;
    if (!device) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* sem = new VkSemaphore_T();
    device->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&sem->fence));
    *pSemaphore = sem;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroySemaphore(
    VkDevice device,
    VkSemaphore semaphore,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete semaphore;
}

// ============================================================================
// Render Pass / Framebuffer (存储元数据, D3D12 无等价概念)
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateRenderPass(
    VkDevice device,
    const VkRenderPassCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass)
{
    (void)device; (void)pAllocator;

    auto* rp = new VkRenderPass_T();
    rp->attachments.assign(pCreateInfo->pAttachments,
                           pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

    // 分析 color/depth attachment
    rp->colorAttachmentCount = 0;
    rp->hasDepth = false;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        auto fmt = pCreateInfo->pAttachments[i].format;
        if (fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D24_UNORM_S8_UINT
            || fmt == VK_FORMAT_D16_UNORM) {
            rp->hasDepth = true;
            rp->depthFormat = fmt;
        } else {
            rp->colorAttachmentCount++;
        }
    }

    *pRenderPass = rp;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyRenderPass(
    VkDevice device,
    VkRenderPass renderPass,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete renderPass;
}

static VkResult VKAPI_CALL d3d12_vkCreateFramebuffer(
    VkDevice device,
    const VkFramebufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFramebuffer* pFramebuffer)
{
    (void)device; (void)pAllocator;

    auto* fb = new VkFramebuffer_T();
    fb->width = pCreateInfo->width;
    fb->height = pCreateInfo->height;
    fb->renderPass = pCreateInfo->renderPass;
    fb->attachments.assign(pCreateInfo->pAttachments,
                           pCreateInfo->pAttachments + pCreateInfo->attachmentCount);
    *pFramebuffer = fb;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyFramebuffer(
    VkDevice device,
    VkFramebuffer framebuffer,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete framebuffer;
}

// ============================================================================
// Pipeline (stub - 完整实现需要 Root Signature + PSO 构建)
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkShaderModule* pShaderModule)
{
    (void)device; (void)pAllocator;
    auto* sm = new VkShaderModule_T();
    sm->bytecode.assign(reinterpret_cast<const char*>(pCreateInfo->pCode),
                        reinterpret_cast<const char*>(pCreateInfo->pCode) + pCreateInfo->codeSize);

    // 检测 SPIR-V magic → 自动替换为预编译的 ImGui DXIL
    if (pCreateInfo->codeSize >= 4 && pCreateInfo->pCode[0] == 0x07230203) {
        sm->isImguiReplacement = true;
        if (pCreateInfo->codeSize > 1000) {
            sm->bytecode.assign(reinterpret_cast<const char*>(imgui_vs_dxil),
                                reinterpret_cast<const char*>(imgui_vs_dxil) + imgui_vs_dxil_size);
        } else {
            sm->bytecode.assign(reinterpret_cast<const char*>(imgui_ps_dxil),
                                reinterpret_cast<const char*>(imgui_ps_dxil) + imgui_ps_dxil_size);
        }
        std::cout << "[VkD3D12] Replaced SPIR-V with ImGui DXIL (" << sm->bytecode.size() << "B)" << std::endl;
    }

    *pShaderModule = sm;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyShaderModule(
    VkDevice device,
    VkShaderModule shaderModule,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete shaderModule;
}

static VkResult VKAPI_CALL d3d12_vkCreatePipelineLayout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineLayout* pPipelineLayout)
{
    (void)device; (void)pAllocator;
    auto* layout = new VkPipelineLayout_T();
    if (pCreateInfo->setLayoutCount > 0) {
        layout->setLayouts.assign(pCreateInfo->pSetLayouts,
                                  pCreateInfo->pSetLayouts + pCreateInfo->setLayoutCount);
    }
    if (pCreateInfo->pushConstantRangeCount > 0) {
        layout->pushConstantRanges.assign(pCreateInfo->pPushConstantRanges,
                                          pCreateInfo->pPushConstantRanges + pCreateInfo->pushConstantRangeCount);
    }
    // Root Signature 在 createGraphicsPipelines 时构建
    *pPipelineLayout = layout;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyPipelineLayout(
    VkDevice device,
    VkPipelineLayout pipelineLayout,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete pipelineLayout;
}

// VkCompareOp → D3D12_COMPARISON_FUNC
static D3D12_COMPARISON_FUNC vkCompareOpToD3D12(VkCompareOp op) {
    switch (op) {
    case VK_COMPARE_OP_NEVER:            return D3D12_COMPARISON_FUNC_NEVER;
    case VK_COMPARE_OP_LESS:             return D3D12_COMPARISON_FUNC_LESS;
    case VK_COMPARE_OP_EQUAL:            return D3D12_COMPARISON_FUNC_EQUAL;
    case VK_COMPARE_OP_LESS_OR_EQUAL:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case VK_COMPARE_OP_GREATER:          return D3D12_COMPARISON_FUNC_GREATER;
    case VK_COMPARE_OP_NOT_EQUAL:        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case VK_COMPARE_OP_ALWAYS:           return D3D12_COMPARISON_FUNC_ALWAYS;
    default:                             return D3D12_COMPARISON_FUNC_LESS;
    }
}

// 构建 D3D12 Root Signature
// Slang DXIL 的 register 布局 (所有资源在 space 0):
//   b0: FrameData (CBV)       → root param [0]
//   b1: MaterialParams (CBV)  → root param [1]
//   b2: PushConstants (root constants) → root param [2]
//   t0-tN: textures (SRV)    → root param [3] (descriptor table)
//   s0-sN: samplers           → static samplers
//
// 对于只有 set0 (per-frame UBO) 的简单 shader (如 Grid, ImGui),
// 使用宽容的布局: b0 CBV + root constants + 可选 SRV table
static void buildRootSignature(VkDevice device, VkPipelineLayout layout) {
    // 统计: UBO 总数 (跨所有 set 顺序编号 b0, b1, ...) 和 SRV 总数
    uint32_t totalCbvCount = 0;
    uint32_t totalSrvCount = 0;
    uint32_t pushConstSize = 0;

    for (auto* setLayout : layout->setLayouts) {
        if (!setLayout) continue;
        for (auto& binding : setLayout->bindings) {
            if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                totalCbvCount++;
            else
                totalSrvCount += binding.descriptorCount;
        }
    }
    for (auto& pc : layout->pushConstantRanges)
        pushConstSize = std::max(pushConstSize, pc.offset + pc.size);
    uint32_t maxCbvRegister = totalCbvCount;

    // 构建 root parameters
    std::vector<D3D12_ROOT_PARAMETER> rootParams;

    // [0..N-1] Root CBVs (b0, b1, ...)
    for (uint32_t i = 0; i < maxCbvRegister; i++) {
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor.ShaderRegister = i;
        param.Descriptor.RegisterSpace = 0;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams.push_back(param);
    }

    // [N] Root constants (push constants → b{maxCbvRegister}, space 0)
    if (pushConstSize > 0) {
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = maxCbvRegister; // 紧跟 CBV 后面
        param.Constants.RegisterSpace = 0;
        param.Constants.Num32BitValues = (pushConstSize + 3) / 4;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams.push_back(param);
    }

    // [N+1] Descriptor table for SRVs (t0-tN)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    if (totalSrvCount > 0) {
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = totalSrvCount;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &srvRange;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams.push_back(param);
    }

    // Static samplers (s0-s3)
    D3D12_STATIC_SAMPLER_DESC samplers[4] = {};
    for (int i = 0; i < 4; i++) {
        samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        samplers[i].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = static_cast<UINT>(rootParams.size());
    rsDesc.pParameters = rootParams.data();
    rsDesc.NumStaticSamplers = 4;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::cerr << "[VkD3D12] RootSig error: " << (char*)err->GetBufferPointer() << std::endl;
        return;
    }

    device->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                         IID_PPV_ARGS(&layout->rootSignature));
    layout->rootParamCount = static_cast<uint32_t>(rootParams.size());
    if (pushConstSize > 0)
        layout->pushConstRootIdx = maxCbvRegister;
    if (totalSrvCount > 0)
        layout->srvTableRootIdx = maxCbvRegister + (pushConstSize > 0 ? 1 : 0);
}

static VkResult VKAPI_CALL d3d12_vkCreateGraphicsPipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines)
{
    (void)pipelineCache; (void)pAllocator;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        auto& ci = pCreateInfos[i];
        auto* pipeline = new VkPipeline_T();
        pipeline->layout = ci.layout;

        // 构建 Root Signature
        if (ci.layout && !ci.layout->rootSignature) {
            buildRootSignature(device, ci.layout);
        }

        // Vertex input → D3D12 input layout + stride 存储
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
        // 检测是否有 ImGui 替换 shader（决定语义名约定）
        bool isImguiPipeline = false;
        for (uint32_t s = 0; s < ci.stageCount; s++) {
            if (ci.pStages[s].module && ci.pStages[s].module->isImguiReplacement)
                isImguiPipeline = true;
        }

        if (ci.pVertexInputState) {
            // 存储 binding stride
            for (uint32_t b = 0; b < ci.pVertexInputState->vertexBindingDescriptionCount; b++) {
                auto& binding = ci.pVertexInputState->pVertexBindingDescriptions[b];
                if (binding.binding < 8) {
                    pipeline->vertexStrides[binding.binding] = binding.stride;
                    if (binding.binding + 1 > pipeline->vertexBindingCount)
                        pipeline->vertexBindingCount = binding.binding + 1;
                }
                fprintf(stderr, "[VkD3D12] CreatePSO pipeline=%p binding[%u] stride=%u imgui=%d\n",
                        pipeline, binding.binding, binding.stride, isImguiPipeline ? 1 : 0);
            }

            // 语义名约定:
            // - ImGui DXIL shader: TEXCOORD{N} (location 0,1,2 → TEXCOORD0,1,2)
            // - 引擎 Slang DXIL:  POSITION/COLOR/TEXCOORD/NORMAL (Slang 默认输出)
            static const char* engineSemantics[] = {
                "POSITION", "COLOR", "TEXCOORD", "NORMAL",
                "TANGENT", "BINORMAL", "BLENDWEIGHT", "BLENDINDICES"
            };
            for (uint32_t a = 0; a < ci.pVertexInputState->vertexAttributeDescriptionCount; a++) {
                auto& attr = ci.pVertexInputState->pVertexAttributeDescriptions[a];
                D3D12_INPUT_ELEMENT_DESC elem = {};
                if (isImguiPipeline) {
                    elem.SemanticName = "TEXCOORD";
                    elem.SemanticIndex = attr.location;
                } else {
                    elem.SemanticName = (attr.location < 8) ? engineSemantics[attr.location] : "TEXCOORD";
                    elem.SemanticIndex = (attr.location >= 4) ? attr.location - 4 : 0;
                }
                elem.Format = vkFormatToDxgi(attr.format);
                elem.InputSlot = attr.binding;
                elem.AlignedByteOffset = attr.offset;
                elem.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                inputLayout.push_back(elem);
                if (!isImguiPipeline) {
                    fprintf(stderr, "[VkD3D12] InputLayout[%u]: semantic=%s idx=%u fmt=%d offset=%u slot=%u\n",
                            a, elem.SemanticName, elem.SemanticIndex, (int)elem.Format,
                            elem.AlignedByteOffset, elem.InputSlot);
                }
            }
        }

        // Shader stages (SPIR-V 在 vkCreateShaderModule 时已替换为 DXIL)
        D3D12_SHADER_BYTECODE vs = {}, ps = {};
        for (uint32_t s = 0; s < ci.stageCount; s++) {
            auto& stage = ci.pStages[s];
            if (!stage.module) continue;
            if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
                vs = {stage.module->bytecode.data(), stage.module->bytecode.size()};
            } else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                ps = {stage.module->bytecode.data(), stage.module->bytecode.size()};
            }
        }

        // Rasterizer state
        D3D12_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_NONE;
        raster.DepthClipEnable = TRUE;
        if (ci.pRasterizationState) {
            raster.FillMode = (ci.pRasterizationState->polygonMode == VK_POLYGON_MODE_LINE)
                ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
            if (ci.pRasterizationState->cullMode == VK_CULL_MODE_BACK_BIT)
                raster.CullMode = D3D12_CULL_MODE_BACK;
            else if (ci.pRasterizationState->cullMode == VK_CULL_MODE_FRONT_BIT)
                raster.CullMode = D3D12_CULL_MODE_FRONT;
            else
                raster.CullMode = D3D12_CULL_MODE_NONE;
            raster.FrontCounterClockwise = (ci.pRasterizationState->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE);
            if (ci.pRasterizationState->depthBiasEnable) {
                raster.DepthBias = static_cast<INT>(ci.pRasterizationState->depthBiasConstantFactor);
                raster.SlopeScaledDepthBias = ci.pRasterizationState->depthBiasSlopeFactor;
                raster.DepthBiasClamp = ci.pRasterizationState->depthBiasClamp;
            }
        }

        // Depth stencil state
        D3D12_DEPTH_STENCIL_DESC depthStencil = {};
        if (ci.pDepthStencilState) {
            depthStencil.DepthEnable = ci.pDepthStencilState->depthTestEnable;
            depthStencil.DepthWriteMask = ci.pDepthStencilState->depthWriteEnable
                ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            depthStencil.DepthFunc = vkCompareOpToD3D12(ci.pDepthStencilState->depthCompareOp);
            depthStencil.StencilEnable = ci.pDepthStencilState->stencilTestEnable;
        }

        // Blend state
        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (ci.pColorBlendState && ci.pColorBlendState->attachmentCount > 0) {
            auto& att = ci.pColorBlendState->pAttachments[0];
            blend.RenderTarget[0].BlendEnable = att.blendEnable;
            blend.RenderTarget[0].RenderTargetWriteMask = att.colorWriteMask & 0xF;
            if (att.blendEnable) {
                auto vkBlendToD3D = [](VkBlendFactor f) -> D3D12_BLEND {
                    switch (f) {
                    case VK_BLEND_FACTOR_ZERO:                return D3D12_BLEND_ZERO;
                    case VK_BLEND_FACTOR_ONE:                 return D3D12_BLEND_ONE;
                    case VK_BLEND_FACTOR_SRC_ALPHA:           return D3D12_BLEND_SRC_ALPHA;
                    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return D3D12_BLEND_INV_SRC_ALPHA;
                    case VK_BLEND_FACTOR_DST_ALPHA:           return D3D12_BLEND_DEST_ALPHA;
                    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return D3D12_BLEND_INV_DEST_ALPHA;
                    case VK_BLEND_FACTOR_SRC_COLOR:           return D3D12_BLEND_SRC_COLOR;
                    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return D3D12_BLEND_INV_SRC_COLOR;
                    case VK_BLEND_FACTOR_DST_COLOR:           return D3D12_BLEND_DEST_COLOR;
                    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return D3D12_BLEND_INV_DEST_COLOR;
                    default: return D3D12_BLEND_ONE;
                    }
                };
                auto vkBlendOpToD3D = [](VkBlendOp op) -> D3D12_BLEND_OP {
                    switch (op) {
                    case VK_BLEND_OP_ADD:              return D3D12_BLEND_OP_ADD;
                    case VK_BLEND_OP_SUBTRACT:         return D3D12_BLEND_OP_SUBTRACT;
                    case VK_BLEND_OP_REVERSE_SUBTRACT: return D3D12_BLEND_OP_REV_SUBTRACT;
                    case VK_BLEND_OP_MIN:              return D3D12_BLEND_OP_MIN;
                    case VK_BLEND_OP_MAX:              return D3D12_BLEND_OP_MAX;
                    default: return D3D12_BLEND_OP_ADD;
                    }
                };
                blend.RenderTarget[0].SrcBlend = vkBlendToD3D(att.srcColorBlendFactor);
                blend.RenderTarget[0].DestBlend = vkBlendToD3D(att.dstColorBlendFactor);
                blend.RenderTarget[0].BlendOp = vkBlendOpToD3D(att.colorBlendOp);
                blend.RenderTarget[0].SrcBlendAlpha = vkBlendToD3D(att.srcAlphaBlendFactor);
                blend.RenderTarget[0].DestBlendAlpha = vkBlendToD3D(att.dstAlphaBlendFactor);
                blend.RenderTarget[0].BlendOpAlpha = vkBlendOpToD3D(att.alphaBlendOp);
            }
        }

        // Render pass 格式信息
        DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
        if (ci.renderPass) {
            auto* rp = ci.renderPass;
            for (auto& att : rp->attachments) {
                DXGI_FORMAT fmt = vkFormatToDxgi(att.format);
                if (fmt == DXGI_FORMAT_D32_FLOAT || fmt == DXGI_FORMAT_D24_UNORM_S8_UINT || fmt == DXGI_FORMAT_D16_UNORM)
                    dsvFormat = fmt;
                else
                    rtvFormat = fmt;
            }
        }

        // Topology
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (ci.pInputAssemblyState) {
            switch (ci.pInputAssemblyState->topology) {
            case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
                topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                pipeline->topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
                break;
            case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
                topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
                pipeline->topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
                break;
            default:
                break;
            }
        }

        // 构建 PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = ci.layout ? ci.layout->rootSignature.Get() : nullptr;
        pso.VS = vs;
        pso.PS = ps;
        pso.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
        pso.PrimitiveTopologyType = topoType;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = rtvFormat;
        pso.DSVFormat = dsvFormat;
        pso.SampleDesc.Count = 1;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState = raster;
        pso.BlendState = blend;
        pso.DepthStencilState = depthStencil;

        // 对于 depth-only render pass，应设置 NumRenderTargets=0
        bool hasColorAttachment = false;
        if (ci.renderPass) {
            for (auto& att : ci.renderPass->attachments) {
                DXGI_FORMAT fmt = vkFormatToDxgi(att.format);
                if (fmt != DXGI_FORMAT_D32_FLOAT && fmt != DXGI_FORMAT_D24_UNORM_S8_UINT && fmt != DXGI_FORMAT_D16_UNORM)
                    hasColorAttachment = true;
            }
        } else {
            hasColorAttachment = true; // 无 render pass 时默认有 color
        }
        if (!hasColorAttachment) {
            pso.NumRenderTargets = 0;
            pso.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        }

        fprintf(stderr, "[VkD3D12] CreatePSO detail: pipeline=%p VS=%zuB PS=%zuB RTVFmt=%d DSVFmt=%d InputElems=%zu NumRT=%d rootSig=%p rootParams=%u\n",
                pipeline, vs.BytecodeLength, ps.BytecodeLength,
                (int)pso.RTVFormats[0], (int)pso.DSVFormat,
                inputLayout.size(), pso.NumRenderTargets,
                (void*)pso.pRootSignature,
                ci.layout ? ci.layout->rootParamCount : 0);
        HRESULT hr = device->device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline->pipelineState));
        if (FAILED(hr)) {
            std::cerr << "[VkD3D12] CreateGraphicsPipelineState failed (HRESULT=0x"
                      << std::hex << hr << std::dec
                      << ", VS=" << vs.BytecodeLength << "B, PS=" << ps.BytecodeLength
                      << "B, InputElems=" << inputLayout.size()
                      << ", RTVFmt=" << rtvFormat << ", DSVFmt=" << dsvFormat
                      << ", NumRT=" << pso.NumRenderTargets
                      << ", hasColor=" << hasColorAttachment
                      << ", rpAttachCount=" << (ci.renderPass ? ci.renderPass->attachments.size() : 0)
                      << ", ImGui=" << isImguiPipeline << ")" << std::endl;
            // 打印每个 attachment 格式
            if (ci.renderPass) {
                for (size_t a = 0; a < ci.renderPass->attachments.size(); a++) {
                    std::cerr << "  att[" << a << "] vkFmt=" << ci.renderPass->attachments[a].format
                              << " dxgiFmt=" << vkFormatToDxgi(ci.renderPass->attachments[a].format) << std::endl;
                }
            }
            flushD3D12Messages(device);
        }

        pPipelines[i] = pipeline;
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyPipeline(
    VkDevice device,
    VkPipeline pipeline,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete pipeline;
}

// ============================================================================
// Descriptor (stub)
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateDescriptorPool(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorPool* pDescriptorPool)
{
    (void)pAllocator;
    auto* pool = new VkDescriptorPool_T();
    pool->device = device;

    // 计算需要的描述符数量
    for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; i++) {
        auto& size = pCreateInfo->pPoolSizes[i];
        if (size.type == VK_DESCRIPTOR_TYPE_SAMPLER)
            pool->maxSamplers += size.descriptorCount;
        else
            pool->maxCbvSrvUav += size.descriptorCount;
    }
    // 至少分配一些
    pool->maxCbvSrvUav = std::max(pool->maxCbvSrvUav, 64u);
    pool->maxSamplers = std::max(pool->maxSamplers, 16u);

    // scratch 区域: 每帧 draw 时合并 SRV 用，预留足够空间
    // 估算: 每帧最多 ~100 draws × 每次 ~4 SRVs = 400
    uint32_t scratchSize = 1024;
    uint32_t totalHeapSize = pool->maxCbvSrvUav + scratchSize;

    // 创建 shader-visible CBV/SRV/UAV heap (包含 scratch 区域)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = totalHeapSize;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&pool->cbvSrvUavHeap));
    pool->srvScratchBase = pool->maxCbvSrvUav;  // scratch 从 set 分配区后面开始
    pool->srvScratchNext = pool->srvScratchBase;

    // 创建 shader-visible Sampler heap
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heapDesc.NumDescriptors = pool->maxSamplers;
    device->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&pool->samplerHeap));

    *pDescriptorPool = pool;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyDescriptorPool(
    VkDevice device,
    VkDescriptorPool descriptorPool,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete descriptorPool;
}

static VkResult VKAPI_CALL d3d12_vkCreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout)
{
    (void)device; (void)pAllocator;
    auto* layout = new VkDescriptorSetLayout_T();
    layout->bindings.assign(pCreateInfo->pBindings,
                            pCreateInfo->pBindings + pCreateInfo->bindingCount);
    *pSetLayout = layout;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyDescriptorSetLayout(
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete descriptorSetLayout;
}

static VkResult VKAPI_CALL d3d12_vkAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets)
{
    (void)device;
    auto* pool = pAllocateInfo->descriptorPool;
    if (!pool) return VK_ERROR_OUT_OF_POOL_MEMORY;

    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
        auto* set = new VkDescriptorSet_T();
        set->pool = pool;
        set->layout = pAllocateInfo->pSetLayouts[i];

        // 计算此 set 需要的描述符数量
        uint32_t descCount = 0;
        if (set->layout) {
            descCount = static_cast<uint32_t>(set->layout->bindings.size());
        }
        descCount = std::max(descCount, 1u);
        set->descriptorCount = descCount;

        // 从 pool 的 shader-visible heap 分配连续区域
        if (pool->cbvSrvUavHeap && pool->allocatedCbvSrvUav + descCount <= pool->maxCbvSrvUav) {
            uint32_t offset = pool->allocatedCbvSrvUav;
            uint32_t incSize = pool->device->cbvSrvUavDescriptorSize;

            auto gpuStart = pool->cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
            auto cpuStart = pool->cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
            set->gpuHandle.ptr = gpuStart.ptr + offset * incSize;
            set->cpuHandle.ptr = cpuStart.ptr + offset * incSize;

            pool->allocatedCbvSrvUav += descCount;
        }

        pDescriptorSets[i] = set;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkFreeDescriptorSets(
    VkDevice device,
    VkDescriptorPool descriptorPool,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets)
{
    (void)device; (void)descriptorPool;
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        delete pDescriptorSets[i];
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkUpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies)
{
    (void)descriptorCopyCount; (void)pDescriptorCopies;
    if (!device) return;

    uint32_t incSize = device->cbvSrvUavDescriptorSize;

    for (uint32_t w = 0; w < descriptorWriteCount; w++) {
        auto& write = pDescriptorWrites[w];
        auto* set = write.dstSet;
        if (!set || set->cpuHandle.ptr == 0) continue;

        // 目标 CPU handle = set 起始 + binding 偏移
        D3D12_CPU_DESCRIPTOR_HANDLE dstHandle = set->cpuHandle;
        dstHandle.ptr += write.dstBinding * incSize;

        for (uint32_t d = 0; d < write.descriptorCount; d++) {
            D3D12_CPU_DESCRIPTOR_HANDLE curHandle = dstHandle;
            curHandle.ptr += d * incSize;

            if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                // UBO → 记录 GPU 虚拟地址 (用于 root CBV 绑定)
                if (write.pBufferInfo) {
                    auto& bufInfo = write.pBufferInfo[d];
                    if (bufInfo.buffer && bufInfo.buffer->resource) {
                        D3D12_GPU_VIRTUAL_ADDRESS addr = bufInfo.buffer->resource->GetGPUVirtualAddress() + bufInfo.offset;
                        // 按 binding index 写入 cbvAddresses
                        uint32_t cbvIdx = write.dstBinding + d;
                        if (cbvIdx < 8) {
                            set->cbvAddresses[cbvIdx] = addr;
                            if (cbvIdx + 1 > set->cbvCount) set->cbvCount = cbvIdx + 1;
                        }
                    }
                }
            } else if (write.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                       write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                // Texture → SRV (从 staging heap 复制到 shader-visible heap)
                if (write.pImageInfo) {
                    auto& imgInfo = write.pImageInfo[d];
                    auto* imageView = imgInfo.imageView;
                    if (imageView && imageView->hasSrv) {
                        device->device->CopyDescriptorsSimple(1, curHandle,
                            imageView->srvHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    }
                }
            }
        }
    }
}

// ============================================================================
// Swapchain (stub)
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
    (void)pAllocator;
    if (!device || !pCreateInfo || !pCreateInfo->surface) return VK_ERROR_INITIALIZATION_FAILED;

    auto* sc = new VkSwapchainKHR_T();
    auto* surface = pCreateInfo->surface;
    uint32_t bufferCount = std::max(pCreateInfo->minImageCount, 2u);

    sc->width = pCreateInfo->imageExtent.width;
    sc->height = pCreateInfo->imageExtent.height;
    sc->format = vkFormatToDxgi(pCreateInfo->imageFormat);
    sc->imageCount = bufferCount;

    // DXGI flip-model 只支持 UNORM，SRGB 通过 RTV view 实现
    DXGI_FORMAT swapChainFormat = sc->format;
    if (swapChainFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        swapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (swapChainFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        swapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // 创建 DXGI SwapChain
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = sc->width;
    desc.Height = sc->height;
    desc.Format = swapChainFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = bufferCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGIFactory4* factory = device->physicalDevice->instance->factory.Get();
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForHwnd(
        device->graphicsQueue.Get(), surface->hwnd,
        &desc, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { delete sc; return VK_ERROR_INITIALIZATION_FAILED; }

    factory->MakeWindowAssociation(surface->hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&sc->swapChain);

    // 创建 RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = bufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    device->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&sc->rtvHeap));
    sc->rtvDescriptorSize = device->rtvDescriptorSize;

    // 获取 back buffer 并创建 RTV + VkImage 句柄
    sc->images.resize(bufferCount);
    sc->imageHandles.resize(bufferCount);
    auto rtvHandle = sc->rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < bufferCount; i++) {
        sc->swapChain->GetBuffer(i, IID_PPV_ARGS(&sc->images[i]));
        // 如果引擎请求 SRGB 格式，RTV 使用 SRGB view（swapchain 用 UNORM 创建）
        if (sc->format != swapChainFormat) {
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = sc->format;  // SRGB format
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->device->CreateRenderTargetView(sc->images[i].Get(), &rtvDesc, rtvHandle);
        } else {
            device->device->CreateRenderTargetView(sc->images[i].Get(), nullptr, rtvHandle);
        }

        // 包装为 VkImage
        auto* img = new VkImage_T();
        img->resource = sc->images[i];
        img->format = pCreateInfo->imageFormat;
        img->width = sc->width;
        img->height = sc->height;
        img->mipLevels = 1;
        img->usage = pCreateInfo->imageUsage;
        img->ownsResource = false;  // swapchain 拥有资源
        img->currentState = D3D12_RESOURCE_STATE_PRESENT;
        img->currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        sc->imageHandles[i] = img;

        rtvHandle.ptr += sc->rtvDescriptorSize;
    }

    sc->currentIndex = sc->swapChain->GetCurrentBackBufferIndex();
    *pSwapchain = sc;
    std::cout << "[VkD3D12] SwapChain created (" << sc->width << "x" << sc->height
              << ", " << bufferCount << " buffers)" << std::endl;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!swapchain) return;
    for (auto* img : swapchain->imageHandles) delete img;
    delete swapchain;
}

static VkResult VKAPI_CALL d3d12_vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages)
{
    (void)device;
    if (!pSwapchainImages) {
        *pSwapchainImageCount = swapchain->imageCount;
        return VK_SUCCESS;
    }
    uint32_t toWrite = std::min(*pSwapchainImageCount, swapchain->imageCount);
    for (uint32_t i = 0; i < toWrite; i++) {
        pSwapchainImages[i] = swapchain->imageHandles[i];
    }
    *pSwapchainImageCount = toWrite;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d12_vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex)
{
    (void)device; (void)timeout; (void)semaphore; (void)fence;
    if (!swapchain || !swapchain->swapChain) return VK_ERROR_SURFACE_LOST_KHR;
    *pImageIndex = swapchain->swapChain->GetCurrentBackBufferIndex();
    swapchain->currentIndex = *pImageIndex;
    // 信号 fence/semaphore (D3D12 中 acquire 是立即的)
    if (fence) fence->signaled = true;
    return VK_SUCCESS;
}

static uint32_t s_presentCount = 0;
static VkResult VKAPI_CALL d3d12_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    (void)queue;
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        auto* sc = pPresentInfo->pSwapchains[i];
        if (sc && sc->swapChain) {
            HRESULT hr = sc->swapChain->Present(1, 0);
            if (s_presentCount < 3) {
                std::cout << "[VkD3D12] Present frame " << s_presentCount
                          << " (hr=0x" << std::hex << hr << std::dec << ")" << std::endl;
            }
            s_presentCount++;
        }
    }
    return VK_SUCCESS;
}

// ============================================================================
// Command Recording (stub - 基础实现)
// ============================================================================

static void VKAPI_CALL d3d12_vkCmdBeginRenderPass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    VkSubpassContents contents)
{
    (void)contents;
    if (!commandBuffer) return;

    auto* rp = pRenderPassBegin->renderPass;
    auto* fb = pRenderPassBegin->framebuffer;
    commandBuffer->currentRenderPass = rp;
    commandBuffer->currentFramebuffer = fb;
    if (!rp || !fb) return;

    auto* cmdList = commandBuffer->commandList.Get();

    static int s_rpCount = 0;
    if (s_rpCount < 5) {
        std::cerr << "[VkD3D12] BeginRenderPass #" << s_rpCount
                  << " attachments=" << fb->attachments.size();
        for (size_t i = 0; i < fb->attachments.size(); i++) {
            auto* v = fb->attachments[i];
            std::cerr << " [" << i << "]:rtv=" << v->hasRtv << ",dsv=" << v->hasDsv;
        }
        std::cerr << std::endl;
    }
    s_rpCount++;

    // 自动插入 barrier: PRESENT/COMMON → RENDER_TARGET / DEPTH_WRITE
    for (size_t i = 0; i < fb->attachments.size(); i++) {
        auto* view = fb->attachments[i];
        if (!view || !view->image || !view->image->resource) continue;
        auto* img = view->image;

        D3D12_RESOURCE_STATES target = D3D12_RESOURCE_STATE_COMMON;
        if (view->hasRtv) target = D3D12_RESOURCE_STATE_RENDER_TARGET;
        else if (view->hasDsv) target = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        else continue;

        if (img->currentState != target) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = img->resource.Get();
            b.Transition.StateBefore = img->currentState;
            b.Transition.StateAfter = target;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &b);
            img->currentState = target;
        }
    }

    // 收集 RTV 和 DSV
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8] = {};
    uint32_t rtvCount = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
    bool hasDsv = false;

    for (size_t i = 0; i < fb->attachments.size(); i++) {
        auto* view = fb->attachments[i];
        if (!view) continue;
        if (view->hasRtv) {
            rtvHandles[rtvCount++] = view->rtvHandle;
        } else if (view->hasDsv) {
            dsvHandle = view->dsvHandle;
            hasDsv = true;
        }
    }

    // 设置 render target
    cmdList->OMSetRenderTargets(rtvCount, rtvCount > 0 ? rtvHandles : nullptr,
                                 FALSE, hasDsv ? &dsvHandle : nullptr);

    // 根据 render pass 的 loadOp 执行 clear
    uint32_t clearIdx = 0;
    for (size_t i = 0; i < fb->attachments.size() && i < rp->attachments.size(); i++) {
        auto* view = fb->attachments[i];
        if (!view) continue;
        auto& att = rp->attachments[i];

        if (view->hasRtv && att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            if (clearIdx < pRenderPassBegin->clearValueCount) {
                auto& cv = pRenderPassBegin->pClearValues[clearIdx];
                cmdList->ClearRenderTargetView(view->rtvHandle, cv.color.float32, 0, nullptr);
            }
        }
        if (view->hasDsv && att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            if (clearIdx < pRenderPassBegin->clearValueCount) {
                auto& cv = pRenderPassBegin->pClearValues[clearIdx];
                cmdList->ClearDepthStencilView(view->dsvHandle, D3D12_CLEAR_FLAG_DEPTH,
                    cv.depthStencil.depth, cv.depthStencil.stencil, 0, nullptr);
            }
        }
        clearIdx++;
    }

    // 设置 viewport 和 scissor（D3D12 要求在设置 render target 后设置）
    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(fb->width);
    vp.Height = static_cast<float>(fb->height);
    vp.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = {0, 0, (LONG)fb->width, (LONG)fb->height};
    cmdList->RSSetScissorRects(1, &scissor);
}

static void VKAPI_CALL d3d12_vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
    if (!commandBuffer) return;

    // 根据 render pass 的 finalLayout 插入 barrier
    auto* rp = commandBuffer->currentRenderPass;
    auto* fb = commandBuffer->currentFramebuffer;
    if (rp && fb) {
        auto* cmdList = commandBuffer->commandList.Get();
        for (size_t i = 0; i < fb->attachments.size() && i < rp->attachments.size(); i++) {
            auto* view = fb->attachments[i];
            if (!view || !view->image || !view->image->resource) continue;
            auto* img = view->image;

            VkImageLayout finalLayout = rp->attachments[i].finalLayout;
            D3D12_RESOURCE_STATES target = D3D12_RESOURCE_STATE_COMMON;
            if (finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                target = D3D12_RESOURCE_STATE_PRESENT;
            else if (finalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                target = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            else if (finalLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
                target = D3D12_RESOURCE_STATE_DEPTH_READ;
            else
                continue; // 其他 layout 不需要转换

            if (img->currentState != target) {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = img->resource.Get();
                b.Transition.StateBefore = img->currentState;
                b.Transition.StateAfter = target;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cmdList->ResourceBarrier(1, &b);
                img->currentState = target;
            }
        }
    }

    commandBuffer->currentRenderPass = VK_NULL_HANDLE;
    commandBuffer->currentFramebuffer = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// 延迟绑定: BindPipeline / BindDescriptorSets 只记录状态,
// flushGraphicsState() 在 Draw 前统一提交到 D3D12
// ---------------------------------------------------------------------------

static void VKAPI_CALL d3d12_vkCmdBindPipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline)
{
    (void)pipelineBindPoint;
    if (!commandBuffer || !pipeline) return;
    commandBuffer->currentPipeline = pipeline;
    commandBuffer->stateDirty = true;
    // PSO / RootSignature / Topology 立即设置 (PushConstants 需要 root sig)
    if (pipeline->pipelineState)
        commandBuffer->commandList->SetPipelineState(pipeline->pipelineState.Get());
    if (pipeline->layout && pipeline->layout->rootSignature)
        commandBuffer->commandList->SetGraphicsRootSignature(pipeline->layout->rootSignature.Get());
    commandBuffer->commandList->IASetPrimitiveTopology(pipeline->topology);
}

static void VKAPI_CALL d3d12_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* pDynamicOffsets)
{
    (void)pipelineBindPoint; (void)dynamicOffsetCount; (void)pDynamicOffsets;
    if (!commandBuffer) return;

    // 只记录状态
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        uint32_t setIdx = firstSet + i;
        if (setIdx < 4) {
            commandBuffer->boundSets[setIdx] = pDescriptorSets[i];
            if (pDescriptorSets[i] && pDescriptorSets[i]->pool)
                commandBuffer->boundPool = pDescriptorSets[i]->pool;
        }
    }
    if (!commandBuffer->boundLayout)
        commandBuffer->boundLayout = layout;
    commandBuffer->stateDirty = true;
}

// 在 Draw 前提交所有 D3D12 状态
static void flushGraphicsState(VkCommandBuffer commandBuffer)
{
    if (!commandBuffer->stateDirty) return;
    commandBuffer->stateDirty = false;

    auto* cmdList = commandBuffer->commandList.Get();
    auto* pipeline = commandBuffer->currentPipeline;
    if (!pipeline) return;

    // PSO / RootSig / Topology 已在 BindPipeline 中设置
    auto* pl = pipeline->layout;
    if (!pl) return;

    // 2. Descriptor Heaps
    if (commandBuffer->boundPool) {
        auto* pool = commandBuffer->boundPool;
        ID3D12DescriptorHeap* heaps[2] = {};
        uint32_t heapCount = 0;
        if (pool->cbvSrvUavHeap) heaps[heapCount++] = pool->cbvSrvUavHeap.Get();
        if (pool->samplerHeap)   heaps[heapCount++] = pool->samplerHeap.Get();
        if (heapCount > 0)
            cmdList->SetDescriptorHeaps(heapCount, heaps);
    }

    // 3. Root CBVs — 遍历所有 set layout，按顺序绑定 UBO
    uint32_t cbvRootIdx = 0;
    for (uint32_t s = 0; s < pl->setLayouts.size() && s < 4; s++) {
        auto* setLayout = pl->setLayouts[s];
        if (!setLayout) continue;
        auto* set = commandBuffer->boundSets[s];

        for (auto& binding : setLayout->bindings) {
            if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                if (set && binding.binding < 8 && set->cbvAddresses[binding.binding] != 0) {
                    cmdList->SetGraphicsRootConstantBufferView(
                        cbvRootIdx, set->cbvAddresses[binding.binding]);
                }
                cbvRootIdx++;
            }
        }
    }

    // 4. SRV Descriptor Table — 合并所有 set 的 SRV 到连续区域
    if (pl->srvTableRootIdx != UINT32_MAX && commandBuffer->boundPool &&
        commandBuffer->boundPool->device) {
        auto* dev = commandBuffer->boundPool->device;
        auto* pool = commandBuffer->boundPool;
        uint32_t incSize = dev->cbvSrvUavDescriptorSize;

        // 统计 SRV 总数
        uint32_t totalSrvs = 0;
        for (auto* setLayout : pl->setLayouts) {
            if (!setLayout) continue;
            for (auto& binding : setLayout->bindings) {
                if (binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                    totalSrvs += binding.descriptorCount;
            }
        }

        if (totalSrvs > 0 && commandBuffer->srvScratchOffset + totalSrvs <= 1024) {
            uint32_t srvStart = pool->srvScratchBase + commandBuffer->srvScratchOffset;
            D3D12_CPU_DESCRIPTOR_HANDLE dstCpuBase = pool->cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
            dstCpuBase.ptr += srvStart * incSize;
            D3D12_GPU_DESCRIPTOR_HANDLE dstGpuBase = pool->cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
            dstGpuBase.ptr += srvStart * incSize;

            uint32_t globalSrvIdx = 0;
            for (uint32_t s = 0; s < pl->setLayouts.size() && s < 4; s++) {
                auto* setLayout = pl->setLayouts[s];
                if (!setLayout) continue;
                auto* set = commandBuffer->boundSets[s];

                for (auto& binding : setLayout->bindings) {
                    if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                        continue;
                    for (uint32_t d = 0; d < binding.descriptorCount; d++) {
                        if (set && set->cpuHandle.ptr != 0) {
                            D3D12_CPU_DESCRIPTOR_HANDLE src = set->cpuHandle;
                            src.ptr += (binding.binding + d) * incSize;
                            D3D12_CPU_DESCRIPTOR_HANDLE dst = dstCpuBase;
                            dst.ptr += globalSrvIdx * incSize;
                            dev->device->CopyDescriptorsSimple(1, dst, src,
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                        }
                        globalSrvIdx++;
                    }
                }
            }

            commandBuffer->srvScratchOffset += totalSrvs;
            cmdList->SetGraphicsRootDescriptorTable(pl->srvTableRootIdx, dstGpuBase);
        }
    }
}

static int s_bindVB = 0;
static void VKAPI_CALL d3d12_vkCmdBindVertexBuffers(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets)
{
    (void)firstBinding;
    if (!commandBuffer) return;
    // 转换为 D3D12 vertex buffer views (stride 从当前绑定的 pipeline 获取)
    std::vector<D3D12_VERTEX_BUFFER_VIEW> views(bindingCount);
    for (uint32_t i = 0; i < bindingCount; i++) {
        if (pBuffers[i] && pBuffers[i]->resource) {
            views[i].BufferLocation = pBuffers[i]->resource->GetGPUVirtualAddress() + pOffsets[i];
            views[i].SizeInBytes = static_cast<UINT>(pBuffers[i]->size - pOffsets[i]);
            uint32_t slot = firstBinding + i;
            views[i].StrideInBytes = 0;
            if (commandBuffer->currentPipeline && slot < 8)
                views[i].StrideInBytes = commandBuffer->currentPipeline->vertexStrides[slot];
        }
    }
    // 验证 VB 数据（每帧前 3 次）
    if (s_bindVB < 30 && views[0].StrideInBytes > 20 && pBuffers[0] && pBuffers[0]->resource) {
        void* peek = nullptr;
        D3D12_RANGE readRange = {0, std::min((UINT64)64, (UINT64)pBuffers[0]->size)};
        if (SUCCEEDED(pBuffers[0]->resource->Map(0, &readRange, &peek))) {
            float* f = reinterpret_cast<float*>(static_cast<uint8_t*>(peek) + pOffsets[0]);
            fprintf(stderr, "[VkD3D12] BindVB pipeline=%p stride=%d size=%llu gpu=0x%llx DATA=[%.3f %.3f %.3f | %.3f %.3f %.3f]\n",
                    commandBuffer->currentPipeline,
                    views[0].StrideInBytes, (unsigned long long)views[0].SizeInBytes,
                    (unsigned long long)views[0].BufferLocation,
                    f[0], f[1], f[2], f[3], f[4], f[5]);
            D3D12_RANGE emptyRange = {0, 0};
            pBuffers[0]->resource->Unmap(0, &emptyRange);
        } else {
            fprintf(stderr, "[VkD3D12] BindVB pipeline=%p stride=%d MAP FAILED\n",
                    commandBuffer->currentPipeline, views[0].StrideInBytes);
        }
    } else {
        fprintf(stderr, "[VkD3D12] BindVB pipeline=%p stride=%d size=%llu gpu=0x%llx\n",
                commandBuffer->currentPipeline,
                views[0].StrideInBytes, (unsigned long long)views[0].SizeInBytes,
                (unsigned long long)views[0].BufferLocation);
    }
    s_bindVB++;
    commandBuffer->commandList->IASetVertexBuffers(firstBinding, bindingCount, views.data());
}

static void VKAPI_CALL d3d12_vkCmdBindIndexBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkIndexType indexType)
{
    if (!commandBuffer || !buffer || !buffer->resource) return;
    D3D12_INDEX_BUFFER_VIEW view = {};
    view.BufferLocation = buffer->resource->GetGPUVirtualAddress() + offset;
    view.SizeInBytes = static_cast<UINT>(buffer->size - offset);
    view.Format = (indexType == VK_INDEX_TYPE_UINT32)
        ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    static int s_ib = 0;
    if (s_ib < 3) fprintf(stderr, "[VkD3D12] BindIndexBuffer #%d size=%u fmt=%s\n", s_ib, view.SizeInBytes, (indexType == VK_INDEX_TYPE_UINT32) ? "u32" : "u16");
    s_ib++;
    commandBuffer->commandList->IASetIndexBuffer(&view);
}

static void VKAPI_CALL d3d12_vkCmdDraw(
    VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance)
{
    if (!commandBuffer) return;
    if (!commandBuffer->currentPipeline || !commandBuffer->currentPipeline->pipelineState) return;
    flushGraphicsState(commandBuffer);
    commandBuffer->commandList->DrawInstanced(vertexCount, instanceCount,
                                               firstVertex, firstInstance);
}

static void VKAPI_CALL d3d12_vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance)
{
    if (!commandBuffer) return;
    if (!commandBuffer->currentPipeline || !commandBuffer->currentPipeline->pipelineState) return;
    flushGraphicsState(commandBuffer);
    commandBuffer->commandList->DrawIndexedInstanced(indexCount, instanceCount,
                                                      firstIndex, vertexOffset, firstInstance);
}

static void VKAPI_CALL d3d12_vkCmdSetViewport(
    VkCommandBuffer commandBuffer,
    uint32_t firstViewport,
    uint32_t viewportCount,
    const VkViewport* pViewports)
{
    (void)firstViewport;
    if (!commandBuffer) return;
    std::vector<D3D12_VIEWPORT> d3dViewports(viewportCount);
    for (uint32_t i = 0; i < viewportCount; i++) {
        d3dViewports[i].TopLeftX = pViewports[i].x;
        d3dViewports[i].TopLeftY = pViewports[i].y;
        d3dViewports[i].Width = pViewports[i].width;
        d3dViewports[i].Height = pViewports[i].height;
        d3dViewports[i].MinDepth = pViewports[i].minDepth;
        d3dViewports[i].MaxDepth = pViewports[i].maxDepth;
    }
    commandBuffer->commandList->RSSetViewports(viewportCount, d3dViewports.data());
}

static void VKAPI_CALL d3d12_vkCmdSetScissor(
    VkCommandBuffer commandBuffer,
    uint32_t firstScissor,
    uint32_t scissorCount,
    const VkRect2D* pScissors)
{
    (void)firstScissor;
    if (!commandBuffer) return;
    std::vector<D3D12_RECT> d3dRects(scissorCount);
    for (uint32_t i = 0; i < scissorCount; i++) {
        d3dRects[i].left = pScissors[i].offset.x;
        d3dRects[i].top = pScissors[i].offset.y;
        d3dRects[i].right = pScissors[i].offset.x + pScissors[i].extent.width;
        d3dRects[i].bottom = pScissors[i].offset.y + pScissors[i].extent.height;
    }
    commandBuffer->commandList->RSSetScissorRects(scissorCount, d3dRects.data());
}

static void VKAPI_CALL d3d12_vkCmdPushConstants(
    VkCommandBuffer commandBuffer,
    VkPipelineLayout layout,
    VkShaderStageFlags stageFlags,
    uint32_t offset,
    uint32_t size,
    const void* pValues)
{
    (void)stageFlags;
    if (!commandBuffer) return;
    // Push constants → D3D12 Root Constants (使用 layout 中预计算的 index)
    uint32_t rootIdx = 2;
    if (layout && layout->pushConstRootIdx != UINT32_MAX)
        rootIdx = layout->pushConstRootIdx;
    commandBuffer->commandList->SetGraphicsRoot32BitConstants(rootIdx, size / 4, pValues, offset / 4);
}

static void VKAPI_CALL d3d12_vkCmdCopyBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkBuffer dstBuffer,
    uint32_t regionCount,
    const VkBufferCopy* pRegions)
{
    if (!commandBuffer || !srcBuffer || !dstBuffer) return;
    if (!srcBuffer->resource || !dstBuffer->resource) {
        fprintf(stderr, "[VkD3D12] CopyBuffer: src=%p dst=%p SKIPPED (null resource)\n",
                (void*)srcBuffer->resource.Get(), (void*)dstBuffer->resource.Get());
        return;
    }
    fprintf(stderr, "[VkD3D12] CopyBuffer: dst=%p gpu=0x%llx size=%llu\n",
            (void*)dstBuffer->resource.Get(),
            (unsigned long long)(dstBuffer->resource ? dstBuffer->resource->GetGPUVirtualAddress() : 0),
            (unsigned long long)(regionCount > 0 ? pRegions[0].size : 0));

    // 调试：检查 staging buffer 是否有数据
    if (srcBuffer->heapType == D3D12_HEAP_TYPE_UPLOAD && regionCount > 0) {
        void* peek = nullptr;
        D3D12_RANGE readRange = {0, std::min((UINT64)16, (UINT64)srcBuffer->size)};
        if (SUCCEEDED(srcBuffer->resource->Map(0, &readRange, &peek))) {
            auto* bytes = static_cast<uint8_t*>(peek) + pRegions[0].srcOffset;
            float* floats = reinterpret_cast<float*>(bytes);
            fprintf(stderr, "[VkD3D12] CopyBuf staging data: %.3f %.3f %.3f %.3f\n",
                    floats[0], floats[1], floats[2], floats[3]);
            srcBuffer->resource->Unmap(0, nullptr);
        }
    }

    // 两个 UPLOAD buffer 之间用 CPU memcpy（UPLOAD 不能做 GPU copy dest）
    if (srcBuffer->heapType == D3D12_HEAP_TYPE_UPLOAD && dstBuffer->heapType == D3D12_HEAP_TYPE_UPLOAD) {
        void *srcData = nullptr, *dstData = nullptr;
        srcBuffer->resource->Map(0, nullptr, &srcData);
        dstBuffer->resource->Map(0, nullptr, &dstData);
        if (srcData && dstData) {
            for (uint32_t i = 0; i < regionCount; i++) {
                memcpy(static_cast<uint8_t*>(dstData) + pRegions[i].dstOffset,
                       static_cast<uint8_t*>(srcData) + pRegions[i].srcOffset,
                       pRegions[i].size);
            }
        }
        D3D12_RANGE emptyRange = {0, 0};
        srcBuffer->resource->Unmap(0, &emptyRange);
        dstBuffer->resource->Unmap(0, nullptr);
    } else {
        for (uint32_t i = 0; i < regionCount; i++) {
            commandBuffer->commandList->CopyBufferRegion(
                dstBuffer->resource.Get(), pRegions[i].dstOffset,
                srcBuffer->resource.Get(), pRegions[i].srcOffset,
                pRegions[i].size);
        }
    }
}

static void VKAPI_CALL d3d12_vkCmdCopyBufferToImage(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkBufferImageCopy* pRegions)
{
    (void)dstImageLayout;
    if (!commandBuffer || !srcBuffer || !dstImage) return;
    if (!srcBuffer->resource || !dstImage->resource) return;

    auto* cmdList = commandBuffer->commandList.Get();
    VkDevice vkDev = commandBuffer->device;

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& region = pRegions[i];
        DXGI_FORMAT fmt = vkFormatToDxgi(dstImage->format);

        // 计算每像素字节数
        uint32_t texelSize = 4;
        if (fmt == DXGI_FORMAT_R8_UNORM) texelSize = 1;
        else if (fmt == DXGI_FORMAT_R8G8_UNORM) texelSize = 2;
        else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) texelSize = 8;
        else if (fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) texelSize = 16;

        uint32_t width = region.imageExtent.width;
        uint32_t height = region.imageExtent.height;
        uint32_t srcRowPitch = (region.bufferRowLength > 0)
            ? region.bufferRowLength * texelSize : width * texelSize;
        uint32_t alignedRowPitch = (srcRowPitch + 255) & ~255u;

        // 源数据的 pitch 和 D3D12 要求的对齐 pitch 不同时，需要中间 buffer
        bool needTempBuf = (alignedRowPitch != srcRowPitch);
        // 也检查 buffer 空间是否够对齐后的大小
        VkDeviceSize availSize = srcBuffer->size - region.bufferOffset;
        if ((VkDeviceSize)alignedRowPitch * height > availSize)
            needTempBuf = true;

        if (needTempBuf && vkDev) {
            // 创建临时对齐 buffer，逐行拷贝
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = (UINT64)alignedRowPitch * height;
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ComPtr<ID3D12Resource> tempBuf;
            vkDev->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tempBuf));

            void *srcData = nullptr, *dstData = nullptr;
            srcBuffer->resource->Map(0, nullptr, &srcData);
            tempBuf->Map(0, nullptr, &dstData);
            if (srcData && dstData) {
                auto* s = static_cast<uint8_t*>(srcData) + region.bufferOffset;
                auto* d = static_cast<uint8_t*>(dstData);
                for (uint32_t row = 0; row < height; row++)
                    memcpy(d + row * alignedRowPitch, s + row * srcRowPitch, srcRowPitch);
            }
            tempBuf->Unmap(0, nullptr);
            srcBuffer->resource->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
            dstLoc.pResource = dstImage->resource.Get();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = region.imageSubresource.mipLevel;

            D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
            srcLoc.pResource = tempBuf.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint.Offset = 0;
            srcLoc.PlacedFootprint.Footprint.Format = fmt;
            srcLoc.PlacedFootprint.Footprint.Width = width;
            srcLoc.PlacedFootprint.Footprint.Height = height;
            srcLoc.PlacedFootprint.Footprint.Depth = 1;
            srcLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

            cmdList->CopyTextureRegion(&dstLoc,
                region.imageOffset.x, region.imageOffset.y, 0, &srcLoc, nullptr);
            deferRelease(std::move(tempBuf));
        } else {
            // pitch 已经 256 对齐且 buffer 足够大，直接复制
            D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
            dstLoc.pResource = dstImage->resource.Get();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = region.imageSubresource.mipLevel;

            D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
            srcLoc.pResource = srcBuffer->resource.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint.Offset = region.bufferOffset;
            srcLoc.PlacedFootprint.Footprint.Format = fmt;
            srcLoc.PlacedFootprint.Footprint.Width = width;
            srcLoc.PlacedFootprint.Footprint.Height = height;
            srcLoc.PlacedFootprint.Footprint.Depth = std::max(region.imageExtent.depth, 1u);
            srcLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

            cmdList->CopyTextureRegion(&dstLoc,
                region.imageOffset.x, region.imageOffset.y, region.imageOffset.z,
                &srcLoc, nullptr);
        }
    }
}

static void VKAPI_CALL d3d12_vkCmdCopyImageToBuffer(
    VkCommandBuffer commandBuffer,
    VkImage srcImage,
    VkImageLayout srcImageLayout,
    VkBuffer dstBuffer,
    uint32_t regionCount,
    const VkBufferImageCopy* pRegions)
{
    (void)srcImageLayout;
    if (!commandBuffer || !srcImage || !dstBuffer) return;
    if (!srcImage->resource || !dstBuffer->resource) return;

    auto* cmdList = commandBuffer->commandList.Get();

    VkDevice vkDev = commandBuffer->device;

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& region = pRegions[i];

        uint32_t texelSize = 4;
        DXGI_FORMAT fmt = vkFormatToDxgi(srcImage->format);
        if (fmt == DXGI_FORMAT_R8_UNORM) texelSize = 1;
        else if (fmt == DXGI_FORMAT_R8G8_UNORM) texelSize = 2;
        else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) texelSize = 8;
        else if (fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) texelSize = 16;

        uint32_t width = region.imageExtent.width;
        uint32_t height = region.imageExtent.height;
        uint32_t rowPitch = width * texelSize;
        uint32_t alignedRowPitch = (rowPitch + 255) & ~255u;

        // 如果 buffer 不够大装对齐后的数据，用临时 buffer
        bool needTempBuf = ((VkDeviceSize)alignedRowPitch * height > dstBuffer->size - region.bufferOffset);

        ID3D12Resource* copyDst = dstBuffer->resource.Get();
        ComPtr<ID3D12Resource> tempBuf;

        if (needTempBuf && vkDev) {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = (UINT64)alignedRowPitch * height;
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            vkDev->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tempBuf));
            copyDst = tempBuf.Get();
        }

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = srcImage->resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = region.imageSubresource.mipLevel;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = copyDst;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = needTempBuf ? 0 : region.bufferOffset;
        dst.PlacedFootprint.Footprint.Format = fmt;
        dst.PlacedFootprint.Footprint.Width = width;
        dst.PlacedFootprint.Footprint.Height = height;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        // 如果用了临时 buffer，逐行复制到目标 buffer
        if (needTempBuf && tempBuf) {
            // 需要先提交当前 cmd，等 GPU 完成，再 CPU 逐行拷贝
            // 更简单的方式：直接用目标 buffer 的对齐 pitch，让调用者处理
            // 但这里我们可以在 GPU 侧用 CopyBufferRegion 逐行拷贝
            for (uint32_t row = 0; row < height; row++) {
                cmdList->CopyBufferRegion(
                    dstBuffer->resource.Get(), region.bufferOffset + (UINT64)row * rowPitch,
                    tempBuf.Get(), (UINT64)row * alignedRowPitch,
                    rowPitch);
            }
            deferRelease(std::move(tempBuf));
        }
    }
}

static void VKAPI_CALL d3d12_vkCmdPipelineBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount,
    const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers)
{
    (void)srcStageMask; (void)dstStageMask; (void)dependencyFlags;
    (void)memoryBarrierCount; (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount; (void)pBufferMemoryBarriers;
    if (!commandBuffer) return;

    // 转换 image barrier → D3D12 resource barrier
    for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
        auto& ib = pImageMemoryBarriers[i];
        auto* image = ib.image;
        if (!image || !image->resource) continue;

        // 简化的 layout → state 映射
        auto layoutToState = [](VkImageLayout layout) -> D3D12_RESOURCE_STATES {
            switch (layout) {
            case VK_IMAGE_LAYOUT_UNDEFINED:                 return D3D12_RESOURCE_STATE_COMMON;
            case VK_IMAGE_LAYOUT_GENERAL:                   return D3D12_RESOURCE_STATE_COMMON;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:  return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:  return D3D12_RESOURCE_STATE_DEPTH_READ;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:      return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:      return D3D12_RESOURCE_STATE_COPY_DEST;
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:           return D3D12_RESOURCE_STATE_PRESENT;
            default:                                        return D3D12_RESOURCE_STATE_COMMON;
            }
        };

        D3D12_RESOURCE_STATES before = (ib.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            ? image->currentState : layoutToState(ib.oldLayout);
        D3D12_RESOURCE_STATES after = layoutToState(ib.newLayout);

        if (before != after) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = image->resource.Get();
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandBuffer->commandList->ResourceBarrier(1, &barrier);
            image->currentState = after;
        }
        image->currentLayout = ib.newLayout;
    }
}

static void VKAPI_CALL d3d12_vkCmdBlitImage(
    VkCommandBuffer commandBuffer,
    VkImage srcImage, VkImageLayout srcImageLayout,
    VkImage dstImage, VkImageLayout dstImageLayout,
    uint32_t regionCount, const VkImageBlit* pRegions,
    VkFilter filter)
{
    (void)srcImageLayout; (void)dstImageLayout; (void)filter;
    if (!commandBuffer || !srcImage || !dstImage) return;
    if (!srcImage->resource || !dstImage->resource) return;

    auto* cmdList = commandBuffer->commandList.Get();

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& region = pRegions[i];
        uint32_t srcW = region.srcOffsets[1].x - region.srcOffsets[0].x;
        uint32_t srcH = region.srcOffsets[1].y - region.srcOffsets[0].y;
        uint32_t dstW = region.dstOffsets[1].x - region.dstOffsets[0].x;
        uint32_t dstH = region.dstOffsets[1].y - region.dstOffsets[0].y;

        if (srcW == dstW && srcH == dstH &&
            region.srcOffsets[0].x == 0 && region.srcOffsets[0].y == 0 &&
            region.dstOffsets[0].x == 0 && region.dstOffsets[0].y == 0 &&
            srcW == srcImage->width && srcH == srcImage->height) {
            // 同尺寸、全图 → CopyResource (最快)
            cmdList->CopyResource(dstImage->resource.Get(), srcImage->resource.Get());
        } else {
            // 区域复制 (不支持缩放，取 min 尺寸)
            uint32_t copyW = std::min(srcW, dstW);
            uint32_t copyH = std::min(srcH, dstH);

            D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
            srcLoc.pResource = srcImage->resource.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = region.srcSubresource.mipLevel;

            D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
            dstLoc.pResource = dstImage->resource.Get();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = region.dstSubresource.mipLevel;

            D3D12_BOX srcBox = {};
            srcBox.left = region.srcOffsets[0].x;
            srcBox.top = region.srcOffsets[0].y;
            srcBox.right = srcBox.left + copyW;
            srcBox.bottom = srcBox.top + copyH;
            srcBox.back = 1;

            cmdList->CopyTextureRegion(&dstLoc,
                region.dstOffsets[0].x, region.dstOffsets[0].y, 0,
                &srcLoc, &srcBox);
        }
    }
}

// ============================================================================
// Debug
// ============================================================================

static VkResult VKAPI_CALL d3d12_vkCreateDebugUtilsMessengerEXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pMessenger)
{
    (void)instance; (void)pAllocator;
    auto* msg = new VkDebugUtilsMessengerEXT_T();
    msg->callback = pCreateInfo->pfnUserCallback;
    msg->userData = pCreateInfo->pUserData;
    msg->severityFilter = pCreateInfo->messageSeverity;
    msg->typeFilter = pCreateInfo->messageType;
    *pMessenger = msg;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d12_vkDestroyDebugUtilsMessengerEXT(
    VkInstance instance,
    VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* pAllocator)
{
    (void)instance; (void)pAllocator;
    delete messenger;
}

// ============================================================================
// vkGetInstanceProcAddr (用于 ImGui 等第三方库加载函数)
// ============================================================================

static PFN_vkVoidFunction VKAPI_CALL d3d12_vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName)
{
    (void)instance;
    // 返回我们的 D3D12 封装函数指针
    #define CHECK_FUNC(fn) if (strcmp(pName, #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(d3d12_##fn)

    CHECK_FUNC(vkCreateInstance);
    CHECK_FUNC(vkDestroyInstance);
    CHECK_FUNC(vkEnumeratePhysicalDevices);
    CHECK_FUNC(vkGetPhysicalDeviceProperties);
    CHECK_FUNC(vkGetPhysicalDeviceFeatures);
    CHECK_FUNC(vkGetPhysicalDeviceFeatures2);
    CHECK_FUNC(vkGetPhysicalDeviceMemoryProperties);
    CHECK_FUNC(vkGetPhysicalDeviceQueueFamilyProperties);
    CHECK_FUNC(vkEnumerateInstanceExtensionProperties);
    CHECK_FUNC(vkEnumerateInstanceLayerProperties);
    CHECK_FUNC(vkEnumerateDeviceExtensionProperties);
    CHECK_FUNC(vkCreateDevice);
    CHECK_FUNC(vkDestroyDevice);
    CHECK_FUNC(vkGetDeviceQueue);
    CHECK_FUNC(vkDeviceWaitIdle);
    CHECK_FUNC(vkDestroySurfaceKHR);
    CHECK_FUNC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    CHECK_FUNC(vkGetPhysicalDeviceSurfaceFormatsKHR);
    CHECK_FUNC(vkGetPhysicalDeviceSurfacePresentModesKHR);
    CHECK_FUNC(vkGetPhysicalDeviceSurfaceSupportKHR);
    CHECK_FUNC(vkCreateSwapchainKHR);
    CHECK_FUNC(vkDestroySwapchainKHR);
    CHECK_FUNC(vkGetSwapchainImagesKHR);
    CHECK_FUNC(vkAcquireNextImageKHR);
    CHECK_FUNC(vkQueuePresentKHR);
    CHECK_FUNC(vkCreateBuffer);
    CHECK_FUNC(vkDestroyBuffer);
    CHECK_FUNC(vkGetBufferMemoryRequirements);
    CHECK_FUNC(vkAllocateMemory);
    CHECK_FUNC(vkFreeMemory);
    CHECK_FUNC(vkBindBufferMemory);
    CHECK_FUNC(vkMapMemory);
    CHECK_FUNC(vkUnmapMemory);
    CHECK_FUNC(vkFlushMappedMemoryRanges);
    CHECK_FUNC(vkCreateImage);
    CHECK_FUNC(vkDestroyImage);
    CHECK_FUNC(vkGetImageMemoryRequirements);
    CHECK_FUNC(vkBindImageMemory);
    CHECK_FUNC(vkCreateImageView);
    CHECK_FUNC(vkDestroyImageView);
    CHECK_FUNC(vkCreateSampler);
    CHECK_FUNC(vkDestroySampler);
    CHECK_FUNC(vkCreateCommandPool);
    CHECK_FUNC(vkDestroyCommandPool);
    CHECK_FUNC(vkResetCommandPool);
    CHECK_FUNC(vkAllocateCommandBuffers);
    CHECK_FUNC(vkFreeCommandBuffers);
    CHECK_FUNC(vkBeginCommandBuffer);
    CHECK_FUNC(vkEndCommandBuffer);
    CHECK_FUNC(vkResetCommandBuffer);
    CHECK_FUNC(vkCmdBeginRenderPass);
    CHECK_FUNC(vkCmdEndRenderPass);
    CHECK_FUNC(vkCmdBindPipeline);
    CHECK_FUNC(vkCmdBindDescriptorSets);
    CHECK_FUNC(vkCmdBindVertexBuffers);
    CHECK_FUNC(vkCmdBindIndexBuffer);
    CHECK_FUNC(vkCmdDraw);
    CHECK_FUNC(vkCmdDrawIndexed);
    CHECK_FUNC(vkCmdSetViewport);
    CHECK_FUNC(vkCmdSetScissor);
    CHECK_FUNC(vkCmdPushConstants);
    CHECK_FUNC(vkCmdCopyBuffer);
    CHECK_FUNC(vkCmdCopyBufferToImage);
    CHECK_FUNC(vkCmdCopyImageToBuffer);
    CHECK_FUNC(vkCmdPipelineBarrier);
    CHECK_FUNC(vkCmdBlitImage);
    CHECK_FUNC(vkWaitForFences);
    CHECK_FUNC(vkResetFences);
    CHECK_FUNC(vkQueueSubmit);
    CHECK_FUNC(vkQueueWaitIdle);
    CHECK_FUNC(vkCreatePipelineLayout);
    CHECK_FUNC(vkDestroyPipelineLayout);
    CHECK_FUNC(vkCreateShaderModule);
    CHECK_FUNC(vkDestroyShaderModule);
    CHECK_FUNC(vkCreateGraphicsPipelines);
    CHECK_FUNC(vkDestroyPipeline);
    CHECK_FUNC(vkCreateRenderPass);
    CHECK_FUNC(vkDestroyRenderPass);
    CHECK_FUNC(vkCreateFramebuffer);
    CHECK_FUNC(vkDestroyFramebuffer);
    CHECK_FUNC(vkCreateDescriptorPool);
    CHECK_FUNC(vkDestroyDescriptorPool);
    CHECK_FUNC(vkCreateDescriptorSetLayout);
    CHECK_FUNC(vkDestroyDescriptorSetLayout);
    CHECK_FUNC(vkAllocateDescriptorSets);
    CHECK_FUNC(vkFreeDescriptorSets);
    CHECK_FUNC(vkUpdateDescriptorSets);
    CHECK_FUNC(vkCreateSemaphore);
    CHECK_FUNC(vkDestroySemaphore);
    CHECK_FUNC(vkCreateFence);
    CHECK_FUNC(vkDestroyFence);
    CHECK_FUNC(vkCreateDebugUtilsMessengerEXT);
    CHECK_FUNC(vkDestroyDebugUtilsMessengerEXT);
    CHECK_FUNC(vkGetInstanceProcAddr);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    CHECK_FUNC(vkCreateWin32SurfaceKHR);
#endif

    #undef CHECK_FUNC
    return nullptr;
}

// ============================================================================
// 分发表加载入口
// ============================================================================

void vkLoadD3D12Dispatch()
{
    #define VK_D3D12(fn) fn = d3d12_##fn

    // Instance
    VK_D3D12(vkCreateInstance);
    VK_D3D12(vkDestroyInstance);
    VK_D3D12(vkEnumerateInstanceExtensionProperties);
    VK_D3D12(vkEnumerateInstanceLayerProperties);
    VK_D3D12(vkGetInstanceProcAddr);

    // Physical Device
    VK_D3D12(vkEnumeratePhysicalDevices);
    VK_D3D12(vkGetPhysicalDeviceProperties);
    VK_D3D12(vkGetPhysicalDeviceFeatures);
    VK_D3D12(vkGetPhysicalDeviceFeatures2);
    VK_D3D12(vkGetPhysicalDeviceMemoryProperties);
    VK_D3D12(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_D3D12(vkEnumerateDeviceExtensionProperties);

    // Device & Queue
    VK_D3D12(vkCreateDevice);
    VK_D3D12(vkDestroyDevice);
    VK_D3D12(vkGetDeviceQueue);
    VK_D3D12(vkDeviceWaitIdle);

    // Surface
    VK_D3D12(vkDestroySurfaceKHR);
    VK_D3D12(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VK_D3D12(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VK_D3D12(vkGetPhysicalDeviceSurfacePresentModesKHR);
    VK_D3D12(vkGetPhysicalDeviceSurfaceSupportKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    VK_D3D12(vkCreateWin32SurfaceKHR);
#endif

    // Swapchain
    VK_D3D12(vkCreateSwapchainKHR);
    VK_D3D12(vkDestroySwapchainKHR);
    VK_D3D12(vkGetSwapchainImagesKHR);
    VK_D3D12(vkAcquireNextImageKHR);
    VK_D3D12(vkQueuePresentKHR);

    // Buffer
    VK_D3D12(vkCreateBuffer);
    VK_D3D12(vkDestroyBuffer);
    VK_D3D12(vkGetBufferMemoryRequirements);

    // Memory
    VK_D3D12(vkAllocateMemory);
    VK_D3D12(vkFreeMemory);
    VK_D3D12(vkBindBufferMemory);
    VK_D3D12(vkMapMemory);
    VK_D3D12(vkUnmapMemory);
    VK_D3D12(vkFlushMappedMemoryRanges);

    // Image
    VK_D3D12(vkCreateImage);
    VK_D3D12(vkDestroyImage);
    VK_D3D12(vkGetImageMemoryRequirements);
    VK_D3D12(vkBindImageMemory);
    VK_D3D12(vkCreateImageView);
    VK_D3D12(vkDestroyImageView);

    // Sampler
    VK_D3D12(vkCreateSampler);
    VK_D3D12(vkDestroySampler);

    // Command Pool & Buffer
    VK_D3D12(vkCreateCommandPool);
    VK_D3D12(vkDestroyCommandPool);
    VK_D3D12(vkResetCommandPool);
    VK_D3D12(vkAllocateCommandBuffers);
    VK_D3D12(vkFreeCommandBuffers);
    VK_D3D12(vkBeginCommandBuffer);
    VK_D3D12(vkEndCommandBuffer);
    VK_D3D12(vkResetCommandBuffer);

    // Command Recording
    VK_D3D12(vkCmdBeginRenderPass);
    VK_D3D12(vkCmdEndRenderPass);
    VK_D3D12(vkCmdBindPipeline);
    VK_D3D12(vkCmdBindDescriptorSets);
    VK_D3D12(vkCmdBindVertexBuffers);
    VK_D3D12(vkCmdBindIndexBuffer);
    VK_D3D12(vkCmdDraw);
    VK_D3D12(vkCmdDrawIndexed);
    VK_D3D12(vkCmdSetViewport);
    VK_D3D12(vkCmdSetScissor);
    VK_D3D12(vkCmdPushConstants);
    VK_D3D12(vkCmdCopyBuffer);
    VK_D3D12(vkCmdCopyBufferToImage);
    VK_D3D12(vkCmdCopyImageToBuffer);
    VK_D3D12(vkCmdPipelineBarrier);
    VK_D3D12(vkCmdBlitImage);

    // Synchronization
    VK_D3D12(vkWaitForFences);
    VK_D3D12(vkResetFences);
    VK_D3D12(vkQueueSubmit);
    VK_D3D12(vkQueueWaitIdle);

    // Pipeline
    VK_D3D12(vkCreatePipelineLayout);
    VK_D3D12(vkDestroyPipelineLayout);
    VK_D3D12(vkCreateShaderModule);
    VK_D3D12(vkDestroyShaderModule);
    VK_D3D12(vkCreateGraphicsPipelines);
    VK_D3D12(vkDestroyPipeline);

    // Render Pass & Framebuffer
    VK_D3D12(vkCreateRenderPass);
    VK_D3D12(vkDestroyRenderPass);
    VK_D3D12(vkCreateFramebuffer);
    VK_D3D12(vkDestroyFramebuffer);

    // Descriptor
    VK_D3D12(vkCreateDescriptorPool);
    VK_D3D12(vkDestroyDescriptorPool);
    VK_D3D12(vkCreateDescriptorSetLayout);
    VK_D3D12(vkDestroyDescriptorSetLayout);
    VK_D3D12(vkAllocateDescriptorSets);
    VK_D3D12(vkFreeDescriptorSets);
    VK_D3D12(vkUpdateDescriptorSets);

    // Sync Objects
    VK_D3D12(vkCreateSemaphore);
    VK_D3D12(vkDestroySemaphore);
    VK_D3D12(vkCreateFence);
    VK_D3D12(vkDestroyFence);

    // Debug
    VK_D3D12(vkCreateDebugUtilsMessengerEXT);
    VK_D3D12(vkDestroyDebugUtilsMessengerEXT);

    #undef VK_D3D12
}

#endif // _WIN32
