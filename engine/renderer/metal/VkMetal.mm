// ============================================================================
// Metal 后端 - VkDispatch 分发层完整实现
//
// 每个 mtl_vkXxx 函数对应一个 Vulkan API，通过 vkLoadMetalDispatch() 注册到
// 全局函数指针表。使用 Metal API (macOS/iOS)。
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#import "renderer/metal/VkMetalHandles.h"
#import "renderer/VkDispatch.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <SDL.h>
#import <SDL_metal.h>
#import <iostream>
#import <cstring>
#import <algorithm>
#import <vector>

// SPIRV-Cross 调用已提取到 SpirvToMSL.cpp（纯 C++，无 ARC）
// 避免 -fobjc-arc 干扰 SPIRV-Cross 内部 C++ 对象的生命周期管理
#include "renderer/metal/SpirvToMSL.h"

// spv:: 枚举仍需在此使用（如 spv::ExecutionModelVertex）
#if __has_include(<spirv_cross/spirv.hpp>)
#include <spirv_cross/spirv.hpp>
#else
#include <spirv.hpp>
#endif

// 句柄转换宏: VkXxx (不透明指针) <-> MTL_Xxx (实际结构体)
#define AS_MTL(Type, handle) reinterpret_cast<MTL_##Type*>(handle)
#define TO_VK(VkType, ptr)  reinterpret_cast<VkType>(ptr)

// SPIR-V magic number (用于检测 ImGui shader 并替换为内置 MSL)
static constexpr uint32_t SPIRV_MAGIC = 0x07230203;

// 全局 SDL_Window 指针
static SDL_Window* s_sdlWindow = nullptr;

// 全局 MTLDevice 指针 (用于 surface capabilities 查询等)
static id<MTLDevice> s_mtlDevice = nil;

// 帧计数器 (诊断用)
static uint32_t s_frameCount = 0;

// 最后提交的 command buffer（用于 QueueWaitIdle / DeviceWaitIdle）
static id<MTLCommandBuffer> s_lastCommittedBuffer = nil;

// ============================================================================
// 格式转换: VkFormat → MTLPixelFormat
// ============================================================================

static MTLPixelFormat toMTLPixelFormat(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_B8G8R8A8_UNORM:          return MTLPixelFormatBGRA8Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB:           return MTLPixelFormatBGRA8Unorm_sRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:          return MTLPixelFormatRGBA8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB:           return MTLPixelFormatRGBA8Unorm_sRGB;
    case VK_FORMAT_R16G16B16A16_SFLOAT:     return MTLPixelFormatRGBA16Float;
    case VK_FORMAT_R32G32B32A32_SFLOAT:     return MTLPixelFormatRGBA32Float;
    case VK_FORMAT_D32_SFLOAT:              return MTLPixelFormatDepth32Float;
    case VK_FORMAT_D24_UNORM_S8_UINT:       return MTLPixelFormatDepth32Float_Stencil8;
    case VK_FORMAT_R8_UNORM:                return MTLPixelFormatR8Unorm;
    case VK_FORMAT_R16_SFLOAT:              return MTLPixelFormatR16Float;
    case VK_FORMAT_R32_SFLOAT:              return MTLPixelFormatR32Float;
    case VK_FORMAT_R8G8_UNORM:              return MTLPixelFormatRG8Unorm;
    default:                                return MTLPixelFormatBGRA8Unorm;
    }
}

// ============================================================================
// 格式转换: VkFormat → MTLVertexFormat
// ============================================================================

static MTLVertexFormat toMTLVertexFormat(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_R32G32_SFLOAT:           return MTLVertexFormatFloat2;
    case VK_FORMAT_R32G32B32_SFLOAT:        return MTLVertexFormatFloat3;
    case VK_FORMAT_R32G32B32A32_SFLOAT:     return MTLVertexFormatFloat4;
    case VK_FORMAT_R8G8B8A8_UNORM:          return MTLVertexFormatUChar4Normalized;
    case VK_FORMAT_R32_SFLOAT:              return MTLVertexFormatFloat;
    case VK_FORMAT_R32_UINT:                return MTLVertexFormatUInt;
    default:                                return MTLVertexFormatFloat3;
    }
}

// ============================================================================
// 辅助: 判断是否为深度格式
// ============================================================================

static bool isDepthFormat(VkFormat fmt)
{
    return fmt == VK_FORMAT_D32_SFLOAT ||
           fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
           fmt == VK_FORMAT_D16_UNORM;
}

// ============================================================================
// 辅助: 获取像素字节大小
// ============================================================================

static uint32_t texelSizeForFormat(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_R8_UNORM:                return 1;
    case VK_FORMAT_R8G8_UNORM:              return 2;
    case VK_FORMAT_R16_SFLOAT:              return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT:              return 4;
    case VK_FORMAT_D24_UNORM_S8_UINT:       return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:     return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:     return 16;
    default:                                return 4;
    }
}

// ============================================================================
// 辅助: VkPrimitiveTopology → MTLPrimitiveType
// ============================================================================

static MTLPrimitiveType toMTLPrimitiveType(VkPrimitiveTopology topo)
{
    switch (topo) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:      return MTLPrimitiveTypePoint;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:       return MTLPrimitiveTypeLine;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:      return MTLPrimitiveTypeLineStrip;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:   return MTLPrimitiveTypeTriangle;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:  return MTLPrimitiveTypeTriangleStrip;
    default:                                    return MTLPrimitiveTypeTriangle;
    }
}

// ============================================================================
// 内置 ImGui MSL 着色器
// ============================================================================

static const char* s_imguiMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ImGuiUniforms {
    float4x4 projectionMatrix;
};

struct VertexIn {
    float2 position [[attribute(0)]];
    float2 texCoord [[attribute(1)]];
    float4 color    [[attribute(2)]]; // UChar4Normalized → 自动转为 [0,1] float4
};

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
    float4 color;
};

vertex VertexOut imgui_vertex(VertexIn in [[stage_in]],
                              constant ImGuiUniforms& uniforms [[buffer(1)]])
{
    VertexOut out;
    out.position = uniforms.projectionMatrix * float4(in.position, 0.0, 1.0);
    out.texCoord = in.texCoord;
    out.color = in.color; // 已经是 [0,1] 范围
    return out;
}

fragment float4 imgui_fragment(VertexOut in [[stage_in]],
                               texture2d<float> tex [[texture(0)]],
                               sampler smp [[sampler(0)]])
{
    float4 texColor = tex.sample(smp, in.texCoord);
    return in.color * texColor;
}
)";

// ============================================================================
// Instance
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateInstance(
    const VkInstanceCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance*                  pInstance)
{
    (void)pCreateInfo; (void)pAllocator;
    auto* inst = new MTL_Instance();
    inst->initialized = true;
    *pInstance = TO_VK(VkInstance, inst);
    std::cout << "[VkMetal] Instance created" << std::endl;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyInstance(
    VkInstance                   instance,
    const VkAllocationCallbacks* pAllocator)
{
    (void)pAllocator;
    if (!instance) return;
    auto* inst = AS_MTL(Instance, instance);
    for (auto pd : inst->physicalDevices)
        delete AS_MTL(PhysicalDevice, pd);
    inst->physicalDevices.clear();
    delete inst;
    std::cout << "[VkMetal] Instance destroyed" << std::endl;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkEnumerateInstanceExtensionProperties(
    const char*              pLayerName,
    uint32_t*                pPropertyCount,
    VkExtensionProperties*   pProperties)
{
    (void)pLayerName;
    const VkExtensionProperties exts[] = {
        {"VK_KHR_surface", 1},
        {"VK_EXT_metal_surface", 1},
    };
    uint32_t count = 2;
    if (!pProperties) { *pPropertyCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pPropertyCount, count);
    memcpy(pProperties, exts, toWrite * sizeof(VkExtensionProperties));
    *pPropertyCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkEnumerateInstanceLayerProperties(
    uint32_t*          pPropertyCount,
    VkLayerProperties* pProperties)
{
    (void)pProperties;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

// 前向声明 debug 函数
static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateDebugUtilsMessengerEXT(
    VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*, const VkAllocationCallbacks*,
    VkDebugUtilsMessengerEXT*);
static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyDebugUtilsMessengerEXT(
    VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mtl_vkGetInstanceProcAddr(
    VkInstance  instance,
    const char* pName)
{
    (void)instance;
    if (!pName) return nullptr;

    // 返回全局注册的函数指针 — ImGui_ImplVulkan 通过此函数加载所有 VK 函数
    #define RET_IF(fn) if (strcmp(pName, #fn) == 0) return (PFN_vkVoidFunction)::fn

    // Instance
    RET_IF(vkCreateInstance);
    RET_IF(vkDestroyInstance);
    RET_IF(vkEnumerateInstanceExtensionProperties);
    RET_IF(vkEnumerateInstanceLayerProperties);
    RET_IF(vkGetInstanceProcAddr);

    // Physical Device
    RET_IF(vkEnumeratePhysicalDevices);
    RET_IF(vkGetPhysicalDeviceProperties);
    RET_IF(vkGetPhysicalDeviceFeatures);
    RET_IF(vkGetPhysicalDeviceFeatures2);
    RET_IF(vkGetPhysicalDeviceMemoryProperties);
    RET_IF(vkGetPhysicalDeviceQueueFamilyProperties);
    RET_IF(vkEnumerateDeviceExtensionProperties);

    // Device & Queue
    RET_IF(vkCreateDevice);
    RET_IF(vkDestroyDevice);
    RET_IF(vkGetDeviceProcAddr);
    RET_IF(vkGetDeviceQueue);
    RET_IF(vkDeviceWaitIdle);

    // Surface
    RET_IF(vkDestroySurfaceKHR);
    RET_IF(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    RET_IF(vkGetPhysicalDeviceSurfaceFormatsKHR);
    RET_IF(vkGetPhysicalDeviceSurfacePresentModesKHR);
    RET_IF(vkGetPhysicalDeviceSurfaceSupportKHR);

    // Swapchain
    RET_IF(vkCreateSwapchainKHR);
    RET_IF(vkDestroySwapchainKHR);
    RET_IF(vkGetSwapchainImagesKHR);
    RET_IF(vkAcquireNextImageKHR);
    RET_IF(vkQueuePresentKHR);

    // Buffer
    RET_IF(vkCreateBuffer);
    RET_IF(vkDestroyBuffer);
    RET_IF(vkGetBufferMemoryRequirements);

    // Memory
    RET_IF(vkAllocateMemory);
    RET_IF(vkFreeMemory);
    RET_IF(vkBindBufferMemory);
    RET_IF(vkMapMemory);
    RET_IF(vkUnmapMemory);
    RET_IF(vkFlushMappedMemoryRanges);

    // Image
    RET_IF(vkCreateImage);
    RET_IF(vkDestroyImage);
    RET_IF(vkGetImageMemoryRequirements);
    RET_IF(vkBindImageMemory);
    RET_IF(vkCreateImageView);
    RET_IF(vkDestroyImageView);

    // Sampler
    RET_IF(vkCreateSampler);
    RET_IF(vkDestroySampler);

    // Command Pool & Buffer
    RET_IF(vkCreateCommandPool);
    RET_IF(vkDestroyCommandPool);
    RET_IF(vkResetCommandPool);
    RET_IF(vkAllocateCommandBuffers);
    RET_IF(vkFreeCommandBuffers);
    RET_IF(vkBeginCommandBuffer);
    RET_IF(vkEndCommandBuffer);
    RET_IF(vkResetCommandBuffer);

    // Command Recording
    RET_IF(vkCmdBeginRenderPass);
    RET_IF(vkCmdEndRenderPass);
    RET_IF(vkCmdBindPipeline);
    RET_IF(vkCmdBindDescriptorSets);
    RET_IF(vkCmdBindVertexBuffers);
    RET_IF(vkCmdBindIndexBuffer);
    RET_IF(vkCmdDraw);
    RET_IF(vkCmdDrawIndexed);
    RET_IF(vkCmdSetViewport);
    RET_IF(vkCmdSetScissor);
    RET_IF(vkCmdPushConstants);
    RET_IF(vkCmdCopyBuffer);
    RET_IF(vkCmdCopyBufferToImage);
    RET_IF(vkCmdCopyImageToBuffer);
    RET_IF(vkCmdPipelineBarrier);
    RET_IF(vkCmdBlitImage);

    // Synchronization
    RET_IF(vkWaitForFences);
    RET_IF(vkResetFences);
    RET_IF(vkQueueSubmit);
    RET_IF(vkQueueWaitIdle);

    // Pipeline
    RET_IF(vkCreatePipelineLayout);
    RET_IF(vkDestroyPipelineLayout);
    RET_IF(vkCreateShaderModule);
    RET_IF(vkDestroyShaderModule);
    RET_IF(vkCreateGraphicsPipelines);
    RET_IF(vkDestroyPipeline);

    // Render Pass & Framebuffer
    RET_IF(vkCreateRenderPass);
    RET_IF(vkDestroyRenderPass);
    RET_IF(vkCreateFramebuffer);
    RET_IF(vkDestroyFramebuffer);

    // Descriptor
    RET_IF(vkCreateDescriptorPool);
    RET_IF(vkDestroyDescriptorPool);
    RET_IF(vkCreateDescriptorSetLayout);
    RET_IF(vkDestroyDescriptorSetLayout);
    RET_IF(vkAllocateDescriptorSets);
    RET_IF(vkFreeDescriptorSets);
    RET_IF(vkUpdateDescriptorSets);

    // Sync Objects
    RET_IF(vkCreateSemaphore);
    RET_IF(vkDestroySemaphore);
    RET_IF(vkCreateFence);
    RET_IF(vkDestroyFence);

    // Debug
    RET_IF(vkCreateDebugUtilsMessengerEXT);
    RET_IF(vkDestroyDebugUtilsMessengerEXT);

    #undef RET_IF

    return nullptr;
}

// ============================================================================
// Physical Device
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkEnumeratePhysicalDevices(
    VkInstance       instance,
    uint32_t*        pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices)
{
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    auto* inst = AS_MTL(Instance, instance);

    // 首次调用时创建物理设备
    if (inst->physicalDevices.empty()) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::cerr << "[VkMetal] ERROR: MTLCreateSystemDefaultDevice() returned nil" << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto* pd = new MTL_PhysicalDevice();
        pd->device = device;
        pd->deviceName = [device.name UTF8String];
        pd->instance = instance;
        s_mtlDevice = device;
        inst->physicalDevices.push_back(TO_VK(VkPhysicalDevice, pd));
    }

    uint32_t count = static_cast<uint32_t>(inst->physicalDevices.size());
    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = count;
        return VK_SUCCESS;
    }
    uint32_t toWrite = std::min(*pPhysicalDeviceCount, count);
    for (uint32_t i = 0; i < toWrite; i++)
        pPhysicalDevices[i] = inst->physicalDevices[i];
    *pPhysicalDeviceCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice          physicalDevice,
    VkPhysicalDeviceProperties* pProperties)
{
    if (!physicalDevice || !pProperties) return;
    auto* pd = AS_MTL(PhysicalDevice, physicalDevice);
    memset(pProperties, 0, sizeof(*pProperties));

    pProperties->apiVersion = VK_API_VERSION_1_0;
    pProperties->driverVersion = 1;
    pProperties->vendorID = 0;
    pProperties->deviceID = 0;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

    if (!pd->deviceName.empty()) {
        strncpy(pProperties->deviceName, pd->deviceName.c_str(),
                VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    } else {
        strncpy(pProperties->deviceName, "Metal GPU",
                VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    }

    pProperties->limits.maxImageDimension2D = 16384;
    pProperties->limits.maxPushConstantsSize = 256;
    pProperties->limits.maxBoundDescriptorSets = 4;
    pProperties->limits.maxPerStageDescriptorSampledImages = 128;
    pProperties->limits.maxPerStageDescriptorSamplers = 16;
    pProperties->limits.minUniformBufferOffsetAlignment = 256;
    pProperties->limits.nonCoherentAtomSize = 64;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice      physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures)
{
    (void)physicalDevice;
    if (!pFeatures) return;
    memset(pFeatures, 0, sizeof(*pFeatures));

    pFeatures->samplerAnisotropy = VK_TRUE;
    pFeatures->fillModeNonSolid = VK_TRUE;
    pFeatures->wideLines = VK_FALSE;
    pFeatures->multiDrawIndirect = VK_FALSE;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice       physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures)
{
    if (!pFeatures) return;
    mtl_vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);

    // 遍历 pNext 链
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(pFeatures->pNext);
    while (next) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            auto* indexing = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(next);
            indexing->shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
            indexing->runtimeDescriptorArray = VK_FALSE;
            indexing->descriptorBindingPartiallyBound = VK_FALSE;
            indexing->descriptorBindingVariableDescriptorCount = VK_FALSE;
            indexing->descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
            indexing->descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
        }
        next = next->pNext;
    }
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice                  physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties)
{
    (void)physicalDevice;
    if (!pMemoryProperties) return;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));

    // Metal 使用 shared memory 模型
    pMemoryProperties->memoryHeapCount = 1;
    pMemoryProperties->memoryHeaps[0].size = 4ULL * 1024 * 1024 * 1024; // 4GB
    pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

    // 3 种 memory type (与其他后端一致)
    pMemoryProperties->memoryTypeCount = 3;

    // Type 0: DEVICE_LOCAL
    pMemoryProperties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;

    // Type 1: HOST_VISIBLE | HOST_COHERENT (上传)
    pMemoryProperties->memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMemoryProperties->memoryTypes[1].heapIndex = 0;

    // Type 2: HOST_VISIBLE | HOST_COHERENT | HOST_CACHED (回读)
    pMemoryProperties->memoryTypes[2].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    pMemoryProperties->memoryTypes[2].heapIndex = 0;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice          physicalDevice,
    uint32_t*                 pQueueFamilyPropertyCount,
    VkQueueFamilyProperties*  pQueueFamilyProperties)
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

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice     physicalDevice,
    const char*          pLayerName,
    uint32_t*            pPropertyCount,
    VkExtensionProperties* pProperties)
{
    (void)physicalDevice; (void)pLayerName;
    const VkExtensionProperties exts[] = {
        {"VK_KHR_swapchain", 1},
    };
    uint32_t count = 1;
    if (!pProperties) { *pPropertyCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pPropertyCount, count);
    memcpy(pProperties, exts, toWrite * sizeof(VkExtensionProperties));
    *pPropertyCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

// ============================================================================
// Device & Queue
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateDevice(
    VkPhysicalDevice             physicalDevice,
    const VkDeviceCreateInfo*    pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice*                    pDevice)
{
    (void)pCreateInfo; (void)pAllocator;
    if (!physicalDevice) return VK_ERROR_INITIALIZATION_FAILED;
    auto* pd = AS_MTL(PhysicalDevice, physicalDevice);

    auto* dev = new MTL_Device();
    dev->physicalDevice = physicalDevice;
    dev->device = pd->device;
    dev->commandQueue = [pd->device newCommandQueue];
    if (!dev->commandQueue) {
        std::cerr << "[VkMetal] ERROR: Failed to create MTLCommandQueue" << std::endl;
        delete dev;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // 获取 SDL_Window
    SDL_Window* window = s_sdlWindow;
    if (!window) {
        window = SDL_GetWindowFromID(1);
    }
    dev->window = window;
    s_sdlWindow = window;
    s_mtlDevice = pd->device;

    *pDevice = TO_VK(VkDevice, dev);
    std::cout << "[VkMetal] Device created: " << pd->deviceName << std::endl;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyDevice(
    VkDevice                     device,
    const VkAllocationCallbacks* pAllocator)
{
    (void)pAllocator;
    if (!device) return;
    auto* dev = AS_MTL(Device, device);
    dev->commandQueue = nil;
    delete dev;
    std::cout << "[VkMetal] Device destroyed" << std::endl;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mtl_vkGetDeviceProcAddr(
    VkDevice    device,
    const char* pName)
{
    (void)device;
    // 委托给 GetInstanceProcAddr
    return mtl_vkGetInstanceProcAddr(VK_NULL_HANDLE, pName);
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkGetDeviceQueue(
    VkDevice  device,
    uint32_t  queueFamilyIndex,
    uint32_t  queueIndex,
    VkQueue*  pQueue)
{
    (void)queueFamilyIndex; (void)queueIndex;
    if (!device || !pQueue) return;
    auto* q = new MTL_Queue();
    q->device = device;
    *pQueue = TO_VK(VkQueue, q);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkDeviceWaitIdle(VkDevice device)
{
    (void)device;
    // 等待最后提交的 command buffer 完成
    if (s_lastCommittedBuffer) {
        [s_lastCommittedBuffer waitUntilCompleted];
        s_lastCommittedBuffer = nil;
    }
    return VK_SUCCESS;
}

// ============================================================================
// Surface
// ============================================================================

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroySurfaceKHR(
    VkInstance                   instance,
    VkSurfaceKHR                 surface,
    const VkAllocationCallbacks* pAllocator)
{
    (void)instance; (void)pAllocator;
    delete AS_MTL(Surface, surface);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice          physicalDevice,
    VkSurfaceKHR              surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
    (void)physicalDevice;
    if (!pSurfaceCapabilities) return VK_ERROR_SURFACE_LOST_KHR;
    auto* surf = surface ? AS_MTL(Surface, surface) : nullptr;

    uint32_t w = 1280, h = 720;
    SDL_Window* win = (surf && surf->window) ? surf->window : s_sdlWindow;
    if (win) {
        int iw, ih;
        SDL_Metal_GetDrawableSize(win, &iw, &ih);
        if (iw <= 0 || ih <= 0) SDL_GetWindowSize(win, &iw, &ih);
        w = static_cast<uint32_t>(iw > 0 ? iw : 1280);
        h = static_cast<uint32_t>(ih > 0 ? ih : 720);
    }

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

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice    physicalDevice,
    VkSurfaceKHR        surface,
    uint32_t*           pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats)
{
    (void)physicalDevice; (void)surface;
    const VkSurfaceFormatKHR formats[] = {
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    uint32_t count = 3;
    if (!pSurfaceFormats) { *pSurfaceFormatCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pSurfaceFormatCount, count);
    memcpy(pSurfaceFormats, formats, toWrite * sizeof(VkSurfaceFormatKHR));
    *pSurfaceFormatCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice  physicalDevice,
    VkSurfaceKHR      surface,
    uint32_t*         pPresentModeCount,
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

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t         queueFamilyIndex,
    VkSurfaceKHR     surface,
    VkBool32*        pSupported)
{
    (void)physicalDevice; (void)queueFamilyIndex; (void)surface;
    *pSupported = VK_TRUE;
    return VK_SUCCESS;
}

// ============================================================================
// Swapchain
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateSwapchainKHR(
    VkDevice                        device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks*    pAllocator,
    VkSwapchainKHR*                 pSwapchain)
{
    (void)pAllocator;
    auto* dev = AS_MTL(Device, device);
    auto* sc = new MTL_Swapchain();

    if (pCreateInfo) {
        sc->width = pCreateInfo->imageExtent.width;
        sc->height = pCreateInfo->imageExtent.height;
        sc->imageCount = std::max(pCreateInfo->minImageCount, 3u);
        sc->format = pCreateInfo->imageFormat;
    }

    // 获取 SDL_Window 并创建 Metal view
    SDL_Window* win = dev->window ? dev->window : s_sdlWindow;
    if (win) {
        sc->metalView = SDL_Metal_CreateView(win);
        if (sc->metalView) {
            sc->metalLayer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(sc->metalView);
            if (sc->metalLayer) {
                sc->metalLayer.device = dev->device;
                sc->metalLayer.pixelFormat = toMTLPixelFormat(sc->format);
                sc->metalLayer.drawableSize = CGSizeMake(sc->width, sc->height);
                sc->metalLayer.maximumDrawableCount = sc->imageCount;
                sc->metalLayer.framebufferOnly = NO;
                // 启用 VSync (macOS only，iOS 默认启用)
#if TARGET_OS_OSX
                sc->metalLayer.displaySyncEnabled = YES;
#endif
            }
        }
    }

    // 为每个 swapchain image 创建占位 MTL_Image
    sc->imageHandles.resize(sc->imageCount);
    for (uint32_t i = 0; i < sc->imageCount; i++) {
        auto* img = new MTL_Image();
        img->format = sc->format;
        img->width = sc->width;
        img->height = sc->height;
        img->ownsResource = false; // 标记为 swapchain image
        sc->imageHandles[i] = TO_VK(VkImage, img);
    }

    *pSwapchain = TO_VK(VkSwapchainKHR, sc);
    SDL_Log("[VkMetal] Swapchain created %ux%u images=%u", sc->width, sc->height, sc->imageCount);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroySwapchainKHR(
    VkDevice                     device,
    VkSwapchainKHR               swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!swapchain) return;
    auto* sc = AS_MTL(Swapchain, swapchain);
    for (auto& img : sc->imageHandles)
        delete AS_MTL(Image, img);
    if (sc->metalView)
        SDL_Metal_DestroyView(sc->metalView);
    delete sc;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkGetSwapchainImagesKHR(
    VkDevice       device,
    VkSwapchainKHR swapchain,
    uint32_t*      pSwapchainImageCount,
    VkImage*       pSwapchainImages)
{
    (void)device;
    if (!swapchain) return VK_ERROR_DEVICE_LOST;
    auto* sc = AS_MTL(Swapchain, swapchain);

    if (!pSwapchainImages) {
        *pSwapchainImageCount = sc->imageCount;
        return VK_SUCCESS;
    }

    uint32_t toWrite = std::min(*pSwapchainImageCount, sc->imageCount);
    for (uint32_t i = 0; i < toWrite; i++)
        pSwapchainImages[i] = sc->imageHandles[i];
    *pSwapchainImageCount = toWrite;
    return (toWrite < sc->imageCount) ? VK_INCOMPLETE : VK_SUCCESS;
}

// 全局 swapchain 引用 (用于 command buffer 中获取 drawable)
static MTL_Swapchain* s_currentSwapchain = nullptr;

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkAcquireNextImageKHR(
    VkDevice       device,
    VkSwapchainKHR swapchain,
    uint64_t       timeout,
    VkSemaphore    semaphore,
    VkFence        fence,
    uint32_t*      pImageIndex)
{
    (void)device; (void)timeout; (void)semaphore; (void)fence;
    if (!swapchain) return VK_ERROR_DEVICE_LOST;
    auto* sc = AS_MTL(Swapchain, swapchain);
    s_currentSwapchain = sc;

    // 获取下一个 drawable
    if (sc->metalLayer) {
        // 确保之前的 command buffer 已完成（释放 drawable 回 pool）
        if (s_lastCommittedBuffer) {
            [s_lastCommittedBuffer waitUntilCompleted];
            s_lastCommittedBuffer = nil;
        }
        sc->currentDrawable = [sc->metalLayer nextDrawable];
        if (!sc->currentDrawable) {
            // drawable 不可用，返回让调用者跳过这一帧
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        // 更新当前 swapchain image 的 texture
        auto* img = AS_MTL(Image, sc->imageHandles[sc->currentIndex]);
        if (img) {
            img->texture = sc->currentDrawable.texture;
        }
    }

    *pImageIndex = sc->currentIndex;
    sc->currentIndex = (sc->currentIndex + 1) % sc->imageCount;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkQueuePresentKHR(
    VkQueue                 queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    (void)queue;
    // Present 在 QueueSubmit 中已通过 presentDrawable 完成
    // 如果还有 pending drawable，这里额外处理
    if (pPresentInfo) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            auto* sc = AS_MTL(Swapchain, pPresentInfo->pSwapchains[i]);
            if (sc && sc->currentDrawable) {
                // drawable 已在 QueueSubmit 中 present
                sc->currentDrawable = nil;
            }
        }
    }
    s_frameCount++;
    return VK_SUCCESS;
}

// ============================================================================
// Buffer
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateBuffer(
    VkDevice                     device,
    const VkBufferCreateInfo*    pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkBuffer*                    pBuffer)
{
    (void)pAllocator;
    auto* dev = AS_MTL(Device, device);
    auto* buf = new MTL_Buffer();
    if (pCreateInfo) {
        buf->size = pCreateInfo->size;
        buf->usage = pCreateInfo->usage;

        // Metal: 直接创建 buffer (shared memory)
        NSUInteger bufSize = std::max((VkDeviceSize)16, pCreateInfo->size);
        buf->buffer = [dev->device newBufferWithLength:bufSize
                                              options:MTLResourceStorageModeShared];
        if (buf->buffer) {
            buf->mapped = [buf->buffer contents];
        }
    }
    *pBuffer = TO_VK(VkBuffer, buf);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyBuffer(
    VkDevice                     device,
    VkBuffer                     buffer,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!buffer) return;
    auto* buf = AS_MTL(Buffer, buffer);
    buf->buffer = nil;
    delete buf;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkGetBufferMemoryRequirements(
    VkDevice                device,
    VkBuffer                buffer,
    VkMemoryRequirements*   pMemoryRequirements)
{
    (void)device;
    if (!buffer || !pMemoryRequirements) return;
    auto* buf = AS_MTL(Buffer, buffer);
    pMemoryRequirements->size = (buf->size + 255) & ~255ULL;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0x7; // type 0, 1, 2
}

// ============================================================================
// Memory
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkAllocateMemory(
    VkDevice                    device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory*             pMemory)
{
    (void)device; (void)pAllocator;
    if (!pAllocateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* mem = new MTL_Memory();
    mem->size = pAllocateInfo->allocationSize;
    mem->memoryTypeIndex = pAllocateInfo->memoryTypeIndex;
    *pMemory = TO_VK(VkDeviceMemory, mem);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkFreeMemory(
    VkDevice                     device,
    VkDeviceMemory               memory,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!memory) return;
    delete AS_MTL(Memory, memory);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkBindBufferMemory(
    VkDevice       device,
    VkBuffer       buffer,
    VkDeviceMemory memory,
    VkDeviceSize   memoryOffset)
{
    (void)device; (void)memoryOffset;
    // Metal buffer 已在 CreateBuffer 中创建，无需额外绑定
    // 但需要记录关联关系，供 MapMemory 使用
    if (memory) {
        auto* mem = AS_MTL(Memory, memory);
        mem->boundBuffer = buffer;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkMapMemory(
    VkDevice         device,
    VkDeviceMemory   memory,
    VkDeviceSize     offset,
    VkDeviceSize     size,
    VkMemoryMapFlags flags,
    void**           ppData)
{
    (void)device; (void)size; (void)flags;
    // 通过 MTL_Memory 找到关联的 buffer，返回其 mapped 指针 + offset
    if (memory) {
        auto* mem = AS_MTL(Memory, memory);
        if (mem->boundBuffer) {
            auto* buf = AS_MTL(Buffer, mem->boundBuffer);
            if (buf->mapped) {
                *ppData = static_cast<uint8_t*>(buf->mapped) + offset;
                return VK_SUCCESS;
            }
        }
    }
    *ppData = nullptr;
    return VK_ERROR_MEMORY_MAP_FAILED;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkUnmapMemory(
    VkDevice       device,
    VkDeviceMemory memory)
{
    (void)device; (void)memory;
    // Metal shared memory 不需要 unmap
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkFlushMappedMemoryRanges(
    VkDevice                   device,
    uint32_t                   memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges)
{
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    // Metal shared memory 自动同步
    return VK_SUCCESS;
}

// ============================================================================
// Image
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateImage(
    VkDevice                     device,
    const VkImageCreateInfo*     pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage*                     pImage)
{
    (void)pAllocator;
    auto* dev = AS_MTL(Device, device);
    auto* img = new MTL_Image();

    if (pCreateInfo) {
        img->format = pCreateInfo->format;
        img->width = pCreateInfo->extent.width;
        img->height = pCreateInfo->extent.height;
        img->mipLevels = pCreateInfo->mipLevels;
        img->usage = pCreateInfo->usage;

        // 创建 Metal 纹理
        MTLTextureDescriptor* desc = [[MTLTextureDescriptor alloc] init];
        desc.textureType = MTLTextureType2D;
        desc.pixelFormat = toMTLPixelFormat(pCreateInfo->format);
        desc.width = pCreateInfo->extent.width;
        desc.height = pCreateInfo->extent.height;
        desc.mipmapLevelCount = pCreateInfo->mipLevels;
        desc.storageMode = MTLStorageModePrivate;

        // 设置用途标志
        MTLTextureUsage usage = MTLTextureUsageShaderRead;
        if (pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
            usage |= MTLTextureUsageRenderTarget;
        if (pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
            usage |= MTLTextureUsageRenderTarget;
        if (pCreateInfo->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            usage |= MTLTextureUsageShaderRead;
        if (pCreateInfo->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            usage |= MTLTextureUsageShaderWrite;
        desc.usage = usage;

        // 深度纹理需要特殊处理
        if (isDepthFormat(pCreateInfo->format)) {
            desc.storageMode = MTLStorageModePrivate;
        } else {
            // 非深度纹理使用 shared 以支持 CPU 上传
            desc.storageMode = MTLStorageModeShared;
        }

        img->texture = [dev->device newTextureWithDescriptor:desc];
    }

    *pImage = TO_VK(VkImage, img);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyImage(
    VkDevice                     device,
    VkImage                      image,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!image) return;
    auto* img = AS_MTL(Image, image);
    if (img->ownsResource) {
        img->texture = nil;
    }
    delete img;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkGetImageMemoryRequirements(
    VkDevice               device,
    VkImage                image,
    VkMemoryRequirements*  pMemoryRequirements)
{
    (void)device;
    if (!image || !pMemoryRequirements) return;
    auto* img = AS_MTL(Image, image);
    uint32_t texelSize = texelSizeForFormat(img->format);
    pMemoryRequirements->size = (VkDeviceSize)img->width * img->height * texelSize * img->mipLevels;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0x7;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkBindImageMemory(
    VkDevice       device,
    VkImage        image,
    VkDeviceMemory memory,
    VkDeviceSize   memoryOffset)
{
    (void)device; (void)memoryOffset;
    // Metal 纹理已在 CreateImage 中创建，无需额外绑定
    // 记录关联关系
    if (memory) {
        auto* mem = AS_MTL(Memory, memory);
        mem->boundImage = image;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateImageView(
    VkDevice                      device,
    const VkImageViewCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks*  pAllocator,
    VkImageView*                  pView)
{
    (void)device; (void)pAllocator;
    auto* view = new MTL_ImageView();
    if (pCreateInfo) {
        view->image = pCreateInfo->image;
        view->format = pCreateInfo->format;
        view->viewType = pCreateInfo->viewType;
        view->subresourceRange = pCreateInfo->subresourceRange;

        // 创建纹理视图
        auto* img = AS_MTL(Image, pCreateInfo->image);
        if (img && img->texture) {
            MTLPixelFormat pixelFmt = toMTLPixelFormat(pCreateInfo->format);
            MTLTextureType texType = MTLTextureType2D;
            NSRange levels = NSMakeRange(
                pCreateInfo->subresourceRange.baseMipLevel,
                pCreateInfo->subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS
                    ? img->mipLevels - pCreateInfo->subresourceRange.baseMipLevel
                    : pCreateInfo->subresourceRange.levelCount);
            NSRange slices = NSMakeRange(0, 1);

            view->textureView = [img->texture newTextureViewWithPixelFormat:pixelFmt
                                                               textureType:texType
                                                                    levels:levels
                                                                    slices:slices];
            // 如果创建视图失败，回退使用原始纹理
            if (!view->textureView) {
                view->textureView = img->texture;
            }
        }
    }
    *pView = TO_VK(VkImageView, view);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyImageView(
    VkDevice                     device,
    VkImageView                  imageView,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!imageView) return;
    auto* view = AS_MTL(ImageView, imageView);
    view->textureView = nil;
    delete view;
}

// ============================================================================
// Sampler
// ============================================================================

static MTLSamplerMinMagFilter toMTLFilter(VkFilter filter)
{
    return (filter == VK_FILTER_NEAREST) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
}

static MTLSamplerMipFilter toMTLMipFilter(VkSamplerMipmapMode mode)
{
    return (mode == VK_SAMPLER_MIPMAP_MODE_NEAREST)
        ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
}

static MTLSamplerAddressMode toMTLAddressMode(VkSamplerAddressMode mode)
{
    switch (mode) {
    case VK_SAMPLER_ADDRESS_MODE_REPEAT:            return MTLSamplerAddressModeRepeat;
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:   return MTLSamplerAddressModeMirrorRepeat;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:     return MTLSamplerAddressModeClampToEdge;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:   return MTLSamplerAddressModeClampToBorderColor;
    default:                                        return MTLSamplerAddressModeRepeat;
    }
}

static MTLCompareFunction toMTLCompareFunc(VkCompareOp op)
{
    switch (op) {
    case VK_COMPARE_OP_NEVER:           return MTLCompareFunctionNever;
    case VK_COMPARE_OP_LESS:            return MTLCompareFunctionLess;
    case VK_COMPARE_OP_EQUAL:           return MTLCompareFunctionEqual;
    case VK_COMPARE_OP_LESS_OR_EQUAL:   return MTLCompareFunctionLessEqual;
    case VK_COMPARE_OP_GREATER:         return MTLCompareFunctionGreater;
    case VK_COMPARE_OP_NOT_EQUAL:       return MTLCompareFunctionNotEqual;
    case VK_COMPARE_OP_GREATER_OR_EQUAL:return MTLCompareFunctionGreaterEqual;
    case VK_COMPARE_OP_ALWAYS:          return MTLCompareFunctionAlways;
    default:                            return MTLCompareFunctionLess;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateSampler(
    VkDevice                     device,
    const VkSamplerCreateInfo*   pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler*                   pSampler)
{
    (void)pAllocator;
    auto* dev = AS_MTL(Device, device);
    auto* samp = new MTL_Sampler();

    MTLSamplerDescriptor* desc = [[MTLSamplerDescriptor alloc] init];
    if (pCreateInfo) {
        desc.magFilter = toMTLFilter(pCreateInfo->magFilter);
        desc.minFilter = toMTLFilter(pCreateInfo->minFilter);
        desc.mipFilter = toMTLMipFilter(pCreateInfo->mipmapMode);
        desc.sAddressMode = toMTLAddressMode(pCreateInfo->addressModeU);
        desc.tAddressMode = toMTLAddressMode(pCreateInfo->addressModeV);
        desc.rAddressMode = toMTLAddressMode(pCreateInfo->addressModeW);
        desc.lodMinClamp = pCreateInfo->minLod;
        desc.lodMaxClamp = pCreateInfo->maxLod;

        if (pCreateInfo->anisotropyEnable && pCreateInfo->maxAnisotropy > 1.0f) {
            desc.maxAnisotropy = (NSUInteger)pCreateInfo->maxAnisotropy;
        }

        if (pCreateInfo->compareEnable) {
            desc.compareFunction = toMTLCompareFunc(pCreateInfo->compareOp);
        }
    }

    samp->sampler = [dev->device newSamplerStateWithDescriptor:desc];
    *pSampler = TO_VK(VkSampler, samp);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroySampler(
    VkDevice                     device,
    VkSampler                    sampler,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!sampler) return;
    auto* s = AS_MTL(Sampler, sampler);
    s->sampler = nil;
    delete s;
}

// ============================================================================
// Command Pool & Buffer
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateCommandPool(
    VkDevice                       device,
    const VkCommandPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*   pAllocator,
    VkCommandPool*                 pCommandPool)
{
    (void)pCreateInfo; (void)pAllocator;
    auto* pool = new MTL_CommandPool();
    pool->device = device;
    *pCommandPool = TO_VK(VkCommandPool, pool);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyCommandPool(
    VkDevice                     device,
    VkCommandPool                commandPool,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_MTL(CommandPool, commandPool);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkResetCommandPool(
    VkDevice                device,
    VkCommandPool           commandPool,
    VkCommandPoolResetFlags flags)
{
    (void)device; (void)commandPool; (void)flags;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkAllocateCommandBuffers(
    VkDevice                         device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer*                 pCommandBuffers)
{
    if (pAllocateInfo && pCommandBuffers) {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            auto* cb = new MTL_CommandBuffer();
            cb->device = device;
            pCommandBuffers[i] = TO_VK(VkCommandBuffer, cb);
        }
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkFreeCommandBuffers(
    VkDevice               device,
    VkCommandPool          commandPool,
    uint32_t               commandBufferCount,
    const VkCommandBuffer* pCommandBuffers)
{
    (void)device; (void)commandPool;
    if (pCommandBuffers) {
        for (uint32_t i = 0; i < commandBufferCount; ++i) {
            auto* cb = AS_MTL(CommandBuffer, pCommandBuffers[i]);
            if (cb) {
                cb->commandBuffer = nil;
                cb->encoder = nil;
                delete cb;
            }
        }
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkBeginCommandBuffer(
    VkCommandBuffer                   commandBuffer,
    const VkCommandBufferBeginInfo*   pBeginInfo)
{
    (void)pBeginInfo;
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb) return VK_SUCCESS;

    // 从 device 的命令队列创建新的命令缓冲
    auto* dev = AS_MTL(Device, cb->device);
    if (dev && dev->commandQueue) {
        cb->commandBuffer = [dev->commandQueue commandBuffer];
    }

    cb->currentPipeline = VK_NULL_HANDLE;
    cb->currentRenderPass = VK_NULL_HANDLE;
    cb->currentFramebuffer = VK_NULL_HANDLE;
    memset(cb->boundSets, 0, sizeof(cb->boundSets));
    cb->pushConstantSize = 0;
    cb->boundIndexBuffer = nil;
    cb->encoder = nil;

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (cb && cb->encoder) {
        [cb->encoder endEncoding];
        cb->encoder = nil;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkResetCommandBuffer(
    VkCommandBuffer          commandBuffer,
    VkCommandBufferResetFlags flags)
{
    (void)flags;
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (cb) {
        cb->encoder = nil;
        cb->commandBuffer = nil;
        cb->currentPipeline = VK_NULL_HANDLE;
        cb->currentRenderPass = VK_NULL_HANDLE;
        cb->currentFramebuffer = VK_NULL_HANDLE;
        memset(cb->boundSets, 0, sizeof(cb->boundSets));
        cb->pushConstantSize = 0;
        cb->boundIndexBuffer = nil;
    }
    return VK_SUCCESS;
}

// ============================================================================
// Command Recording
// ============================================================================

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdBeginRenderPass(
    VkCommandBuffer               commandBuffer,
    const VkRenderPassBeginInfo*  pRenderPassBegin,
    VkSubpassContents             contents)
{
    (void)contents;
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !pRenderPassBegin) return;

    auto* rp = AS_MTL(RenderPass, pRenderPassBegin->renderPass);
    auto* fb = AS_MTL(Framebuffer, pRenderPassBegin->framebuffer);
    cb->currentRenderPass = pRenderPassBegin->renderPass;
    cb->currentFramebuffer = pRenderPassBegin->framebuffer;
    if (!rp || !fb) return;

    // 构建 MTLRenderPassDescriptor
    MTLRenderPassDescriptor* rpDesc = [MTLRenderPassDescriptor renderPassDescriptor];

    uint32_t colorIdx = 0;
    uint32_t clearIdx = 0;
    for (size_t i = 0; i < fb->attachments.size() && i < rp->attachments.size(); i++) {
        auto& att = rp->attachments[i];
        auto* view = AS_MTL(ImageView, fb->attachments[i]);
        if (!view) { clearIdx++; continue; }

        auto* img = AS_MTL(Image, view->image);
        id<MTLTexture> tex = view->textureView ? view->textureView : (img ? img->texture : nil);

        // Swapchain image 特殊处理：使用当前 drawable 的 texture
        if (img && !img->ownsResource && s_currentSwapchain && s_currentSwapchain->currentDrawable) {
            tex = s_currentSwapchain->currentDrawable.texture;
        }

        if (isDepthFormat(att.format)) {
            rpDesc.depthAttachment.texture = tex;
            rpDesc.depthAttachment.loadAction =
                (att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) ? MTLLoadActionClear : MTLLoadActionLoad;
            rpDesc.depthAttachment.storeAction = MTLStoreActionStore;
            if (att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && clearIdx < pRenderPassBegin->clearValueCount) {
                rpDesc.depthAttachment.clearDepth = pRenderPassBegin->pClearValues[clearIdx].depthStencil.depth;
            }
            // 如果有 stencil 分量
            if (att.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                rpDesc.stencilAttachment.texture = tex;
                rpDesc.stencilAttachment.loadAction = rpDesc.depthAttachment.loadAction;
                rpDesc.stencilAttachment.storeAction = MTLStoreActionStore;
                if (att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && clearIdx < pRenderPassBegin->clearValueCount) {
                    rpDesc.stencilAttachment.clearStencil = pRenderPassBegin->pClearValues[clearIdx].depthStencil.stencil;
                }
            }
        } else {
            rpDesc.colorAttachments[colorIdx].texture = tex;
            rpDesc.colorAttachments[colorIdx].loadAction =
                (att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) ? MTLLoadActionClear : MTLLoadActionLoad;
            rpDesc.colorAttachments[colorIdx].storeAction = MTLStoreActionStore;
            if (att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && clearIdx < pRenderPassBegin->clearValueCount) {
                auto& cv = pRenderPassBegin->pClearValues[clearIdx];
                rpDesc.colorAttachments[colorIdx].clearColor = MTLClearColorMake(
                    cv.color.float32[0], cv.color.float32[1],
                    cv.color.float32[2], cv.color.float32[3]);
            }
            colorIdx++;
        }
        clearIdx++;
    }

    // 创建 render command encoder
    if (cb->commandBuffer) {
        cb->encoder = [cb->commandBuffer renderCommandEncoderWithDescriptor:rpDesc];
    }
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb) return;

    if (cb->encoder) {
        [cb->encoder endEncoding];
        cb->encoder = nil;
    }

    cb->currentRenderPass = VK_NULL_HANDLE;
    cb->currentFramebuffer = VK_NULL_HANDLE;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdBindPipeline(
    VkCommandBuffer     commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline          pipeline)
{
    (void)pipelineBindPoint;
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->encoder) return;

    cb->currentPipeline = pipeline;
    auto* pipe = AS_MTL(Pipeline, pipeline);
    if (!pipe) return;

    // 设置渲染管线状态（pipelineState 可能因创建失败为 nil，需跳过）
    if (!pipe->pipelineState) return;
    [cb->encoder setRenderPipelineState:pipe->pipelineState];

    // 设置深度模板状态
    if (pipe->depthStencilState) {
        [cb->encoder setDepthStencilState:pipe->depthStencilState];
    }

    // 设置光栅化状态
    [cb->encoder setCullMode:(MTLCullMode)pipe->cullMode];
    [cb->encoder setFrontFacingWinding:(MTLWinding)pipe->frontFace];

    // 深度偏移
    if (pipe->depthBiasEnable) {
        [cb->encoder setDepthBias:pipe->depthBiasConstant
                       slopeScale:pipe->depthBiasSlope
                            clamp:pipe->depthBiasClamp];
    }
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdBindDescriptorSets(
    VkCommandBuffer        commandBuffer,
    VkPipelineBindPoint    pipelineBindPoint,
    VkPipelineLayout       layout,
    uint32_t               firstSet,
    uint32_t               descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t               dynamicOffsetCount,
    const uint32_t*        pDynamicOffsets)
{
    (void)pipelineBindPoint; (void)layout; (void)dynamicOffsetCount; (void)pDynamicOffsets;
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !pDescriptorSets) return;

    for (uint32_t i = 0; i < descriptorSetCount && (firstSet + i) < 4; ++i) {
        cb->boundSets[firstSet + i] = pDescriptorSets[i];
    }

    // 立即绑定资源到 encoder
    if (!cb->encoder) return;

    auto* pipe = AS_MTL(Pipeline, cb->currentPipeline);
    if (!pipe || !pipe->layout) return;
    auto* pl = AS_MTL(PipelineLayout, pipe->layout);
    if (!pl) return;

    // 使用 SPIRV-Cross 的资源映射绑定到正确的 Metal index
    {
        for (uint32_t s = 0; s < pl->setLayouts.size() && s < 4; s++) {
            auto* setLayout = AS_MTL(DescriptorSetLayout, pl->setLayouts[s]);
            if (!setLayout) continue;
            auto* dset = AS_MTL(DescriptorSet, cb->boundSets[s]);
            if (!dset) continue;

            for (auto& binding : setLayout->bindings) {
                auto key = std::make_pair(s, binding.binding);

                if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                    binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                    if (binding.binding < 8) {
                        auto* buf = AS_MTL(Buffer, dset->uboVkBuffers[binding.binding]);
                        if (buf && buf->buffer) {
                            // 查询 VS 的 Metal buffer index
                            auto itVs = pipe->vsBindings.uboBindings.find(key);
                            if (itVs != pipe->vsBindings.uboBindings.end() &&
                                itVs->second != UINT32_MAX) {
                                [cb->encoder setVertexBuffer:buf->buffer
                                                      offset:0
                                                     atIndex:itVs->second];
                            }
                            // 查询 FS 的 Metal buffer index
                            auto itFs = pipe->fsBindings.uboBindings.find(key);
                            if (itFs != pipe->fsBindings.uboBindings.end() &&
                                itFs->second != UINT32_MAX) {
                                [cb->encoder setFragmentBuffer:buf->buffer
                                                        offset:0
                                                       atIndex:itFs->second];
                            }
                        }
                    }
                } else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                           binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                    for (uint32_t d = 0; d < binding.descriptorCount; d++) {
                        uint32_t bIdx = binding.binding + d;
                        auto texKey = std::make_pair(s, bIdx);
                        if (bIdx < 8) {
                            auto* view = AS_MTL(ImageView, dset->imageViews[bIdx]);
                            auto* smp = AS_MTL(Sampler, dset->samplers[bIdx]);

                            id<MTLTexture> tex = nil;
                            if (view) {
                                tex = view->textureView;
                                if (!tex) {
                                    auto* img = AS_MTL(Image, view->image);
                                    if (img) tex = img->texture;
                                }
                            }

                            // VS texture/sampler binding
                            auto itVsTex = pipe->vsBindings.textureBindings.find(texKey);
                            if (itVsTex != pipe->vsBindings.textureBindings.end() &&
                                itVsTex->second != UINT32_MAX) {
                                if (tex)
                                    [cb->encoder setVertexTexture:tex atIndex:itVsTex->second];
                                auto itVsSmp = pipe->vsBindings.samplerBindings.find(texKey);
                                if (itVsSmp != pipe->vsBindings.samplerBindings.end() &&
                                    itVsSmp->second != UINT32_MAX && smp && smp->sampler) {
                                    [cb->encoder setVertexSamplerState:smp->sampler
                                                               atIndex:itVsSmp->second];
                                }
                            }

                            // FS texture/sampler binding
                            auto itFsTex = pipe->fsBindings.textureBindings.find(texKey);
                            if (itFsTex != pipe->fsBindings.textureBindings.end() &&
                                itFsTex->second != UINT32_MAX) {
                                if (tex)
                                    [cb->encoder setFragmentTexture:tex atIndex:itFsTex->second];
                                auto itFsSmp = pipe->fsBindings.samplerBindings.find(texKey);
                                if (itFsSmp != pipe->fsBindings.samplerBindings.end() &&
                                    itFsSmp->second != UINT32_MAX && smp && smp->sampler) {
                                    [cb->encoder setFragmentSamplerState:smp->sampler
                                                                 atIndex:itFsSmp->second];
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdBindVertexBuffers(
    VkCommandBuffer     commandBuffer,
    uint32_t            firstBinding,
    uint32_t            bindingCount,
    const VkBuffer*     pBuffers,
    const VkDeviceSize* pOffsets)
{
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->encoder) return;

    // 确定 SPIRV-Cross 分配的顶点缓冲 Metal buffer base index
    uint32_t vbBaseIndex = 0;
    auto* pipe = AS_MTL(Pipeline, cb->currentPipeline);
    if (pipe && pipe->vsBindings.hasVertexInputs) {
        vbBaseIndex = pipe->vsBindings.vertexBufferIndex;
    }

    for (uint32_t i = 0; i < bindingCount; i++) {
        auto* buf = AS_MTL(Buffer, pBuffers[i]);
        if (!buf || !buf->buffer) continue;

        // Vulkan binding → Metal buffer index (加上 SPIRV-Cross 的偏移)
        uint32_t slot = vbBaseIndex + firstBinding + i;
        [cb->encoder setVertexBuffer:buf->buffer
                              offset:(NSUInteger)pOffsets[i]
                             atIndex:slot];
    }
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdBindIndexBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer        buffer,
    VkDeviceSize    offset,
    VkIndexType     indexType)
{
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb) return;
    auto* buf = AS_MTL(Buffer, buffer);
    if (!buf) return;

    cb->boundIndexBuffer = buf->buffer;
    cb->indexType = indexType;
    cb->indexBufferOffset = offset;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdPushConstants(
    VkCommandBuffer    commandBuffer,
    VkPipelineLayout   layout,
    VkShaderStageFlags stageFlags,
    uint32_t           offset,
    uint32_t           size,
    const void*        pValues)
{
    (void)layout;
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->encoder || !pValues) return;

    // 更新 push constant 缓冲
    if ((offset + size) <= 256) {
        memcpy(cb->pushConstantData + offset, pValues, size);
        if (offset + size > cb->pushConstantSize)
            cb->pushConstantSize = offset + size;
    }

    // 查询 SPIRV-Cross 分配的 push constant Metal buffer index
    auto* pipe = AS_MTL(Pipeline, cb->currentPipeline);
    uint32_t vsPcIdx = 1;  // 默认 index (fallback)
    uint32_t fsPcIdx = 1;
    if (pipe) {
        if (pipe->vsBindings.pushConstantBufferIndex != UINT32_MAX)
            vsPcIdx = pipe->vsBindings.pushConstantBufferIndex;
        if (pipe->fsBindings.pushConstantBufferIndex != UINT32_MAX)
            fsPcIdx = pipe->fsBindings.pushConstantBufferIndex;
    }

    if (stageFlags & VK_SHADER_STAGE_VERTEX_BIT) {
        [cb->encoder setVertexBytes:cb->pushConstantData
                             length:cb->pushConstantSize
                            atIndex:vsPcIdx];
    }
    if (stageFlags & VK_SHADER_STAGE_FRAGMENT_BIT) {
        [cb->encoder setFragmentBytes:cb->pushConstantData
                               length:cb->pushConstantSize
                              atIndex:fsPcIdx];
    }
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdDraw(
    VkCommandBuffer commandBuffer,
    uint32_t        vertexCount,
    uint32_t        instanceCount,
    uint32_t        firstVertex,
    uint32_t        firstInstance)
{
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->encoder || !cb->currentPipeline) return;
    auto* pipe = AS_MTL(Pipeline, cb->currentPipeline);
    if (!pipe || !pipe->pipelineState) return;  // 跳过创建失败的 pipeline

    MTLPrimitiveType primType = (MTLPrimitiveType)pipe->topology;

    if (instanceCount > 1 || firstInstance > 0) {
        [cb->encoder drawPrimitives:primType
                        vertexStart:firstVertex
                        vertexCount:vertexCount
                      instanceCount:instanceCount
                       baseInstance:firstInstance];
    } else {
        [cb->encoder drawPrimitives:primType
                        vertexStart:firstVertex
                        vertexCount:vertexCount];
    }
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer,
    uint32_t        indexCount,
    uint32_t        instanceCount,
    uint32_t        firstIndex,
    int32_t         vertexOffset,
    uint32_t        firstInstance)
{
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->encoder || !cb->currentPipeline) return;
    auto* pipe = AS_MTL(Pipeline, cb->currentPipeline);
    if (!pipe || !pipe->pipelineState || !cb->boundIndexBuffer) return;  // 跳过创建失败的 pipeline

    MTLPrimitiveType primType = (MTLPrimitiveType)pipe->topology;
    MTLIndexType idxType = (cb->indexType == VK_INDEX_TYPE_UINT16)
        ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    uint32_t idxStride = (cb->indexType == VK_INDEX_TYPE_UINT16) ? 2 : 4;
    NSUInteger idxOffset = cb->indexBufferOffset + (NSUInteger)firstIndex * idxStride;

    if (instanceCount > 1 || firstInstance > 0 || vertexOffset != 0) {
        [cb->encoder drawIndexedPrimitives:primType
                                indexCount:indexCount
                                 indexType:idxType
                               indexBuffer:cb->boundIndexBuffer
                         indexBufferOffset:idxOffset
                             instanceCount:instanceCount
                                baseVertex:vertexOffset
                              baseInstance:firstInstance];
    } else {
        [cb->encoder drawIndexedPrimitives:primType
                                indexCount:indexCount
                                 indexType:idxType
                               indexBuffer:cb->boundIndexBuffer
                         indexBufferOffset:idxOffset];
    }
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdSetViewport(
    VkCommandBuffer  commandBuffer,
    uint32_t         firstViewport,
    uint32_t         viewportCount,
    const VkViewport* pViewports)
{
    (void)firstViewport;
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->encoder || !pViewports || viewportCount == 0) return;

    float x = pViewports[0].x;
    float y = pViewports[0].y;
    float w = pViewports[0].width;
    float h = pViewports[0].height;

    // 处理 Vulkan Y-flip (height < 0)
    if (h < 0) {
        y = y + h;
        h = -h;
    }

    // Vulkan NDC Y 向下, Metal NDC Y 向上
    // 翻转 viewport Y: 从底部开始，向上渲染
    MTLViewport vp;
    vp.originX = x;
    vp.originY = y + h; // 翻转: 从底边开始
    vp.width = w;
    vp.height = -h;     // 负高度 = Y 向上
    vp.znear = pViewports[0].minDepth;
    vp.zfar = pViewports[0].maxDepth;
    [cb->encoder setViewport:vp];
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdSetScissor(
    VkCommandBuffer commandBuffer,
    uint32_t        firstScissor,
    uint32_t        scissorCount,
    const VkRect2D* pScissors)
{
    (void)firstScissor;
    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->encoder || !pScissors || scissorCount == 0) return;

    MTLScissorRect rect;
    rect.x = pScissors[0].offset.x;
    rect.y = pScissors[0].offset.y;
    rect.width = pScissors[0].extent.width;
    rect.height = pScissors[0].extent.height;
    [cb->encoder setScissorRect:rect];
}

// ============================================================================
// Copy 命令
// ============================================================================

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdCopyBuffer(
    VkCommandBuffer      commandBuffer,
    VkBuffer             srcBuffer,
    VkBuffer             dstBuffer,
    uint32_t             regionCount,
    const VkBufferCopy*  pRegions)
{
    (void)commandBuffer;
    auto* src = AS_MTL(Buffer, srcBuffer);
    auto* dst = AS_MTL(Buffer, dstBuffer);
    if (!src || !dst || !pRegions) return;

    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->commandBuffer) return;

    // 需要 blit command encoder
    if (cb->encoder) {
        [cb->encoder endEncoding];
        cb->encoder = nil;
    }

    id<MTLBlitCommandEncoder> blit = [cb->commandBuffer blitCommandEncoder];
    for (uint32_t i = 0; i < regionCount; i++) {
        [blit copyFromBuffer:src->buffer
                sourceOffset:(NSUInteger)pRegions[i].srcOffset
                    toBuffer:dst->buffer
           destinationOffset:(NSUInteger)pRegions[i].dstOffset
                        size:(NSUInteger)pRegions[i].size];
    }
    [blit endEncoding];
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdCopyBufferToImage(
    VkCommandBuffer          commandBuffer,
    VkBuffer                 srcBuffer,
    VkImage                  dstImage,
    VkImageLayout            dstImageLayout,
    uint32_t                 regionCount,
    const VkBufferImageCopy* pRegions)
{
    (void)dstImageLayout;
    auto* src = AS_MTL(Buffer, srcBuffer);
    auto* dst = AS_MTL(Image, dstImage);
    if (!src || !dst || !pRegions) return;

    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->commandBuffer) return;

    // 需要 blit command encoder
    if (cb->encoder) {
        [cb->encoder endEncoding];
        cb->encoder = nil;
    }

    id<MTLBlitCommandEncoder> blit = [cb->commandBuffer blitCommandEncoder];
    uint32_t texelSize = texelSizeForFormat(dst->format);

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& r = pRegions[i];
        uint32_t w = r.imageExtent.width;
        uint32_t h = r.imageExtent.height;
        uint32_t bytesPerRow = w * texelSize;
        if (r.bufferRowLength > 0) {
            bytesPerRow = r.bufferRowLength * texelSize;
        }

        [blit copyFromBuffer:src->buffer
                sourceOffset:(NSUInteger)r.bufferOffset
           sourceBytesPerRow:bytesPerRow
         sourceBytesPerImage:bytesPerRow * h
                  sourceSize:MTLSizeMake(w, h, 1)
                   toTexture:dst->texture
            destinationSlice:0
            destinationLevel:r.imageSubresource.mipLevel
           destinationOrigin:MTLOriginMake(r.imageOffset.x, r.imageOffset.y, 0)];
    }
    [blit endEncoding];
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdCopyImageToBuffer(
    VkCommandBuffer          commandBuffer,
    VkImage                  srcImage,
    VkImageLayout            srcImageLayout,
    VkBuffer                 dstBuffer,
    uint32_t                 regionCount,
    const VkBufferImageCopy* pRegions)
{
    (void)srcImageLayout;
    auto* src = AS_MTL(Image, srcImage);
    auto* dst = AS_MTL(Buffer, dstBuffer);
    if (!src || !dst || !pRegions) return;

    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->commandBuffer) return;

    if (cb->encoder) {
        [cb->encoder endEncoding];
        cb->encoder = nil;
    }

    id<MTLBlitCommandEncoder> blit = [cb->commandBuffer blitCommandEncoder];
    uint32_t texelSize = texelSizeForFormat(src->format);

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& r = pRegions[i];
        uint32_t w = r.imageExtent.width;
        uint32_t h = r.imageExtent.height;
        uint32_t bytesPerRow = w * texelSize;

        [blit copyFromTexture:src->texture
                  sourceSlice:0
                  sourceLevel:r.imageSubresource.mipLevel
                 sourceOrigin:MTLOriginMake(r.imageOffset.x, r.imageOffset.y, 0)
                   sourceSize:MTLSizeMake(w, h, 1)
                     toBuffer:dst->buffer
            destinationOffset:(NSUInteger)r.bufferOffset
       destinationBytesPerRow:bytesPerRow
     destinationBytesPerImage:bytesPerRow * h];
    }
    [blit endEncoding];
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdPipelineBarrier(
    VkCommandBuffer             commandBuffer,
    VkPipelineStageFlags        srcStageMask,
    VkPipelineStageFlags        dstStageMask,
    VkDependencyFlags           dependencyFlags,
    uint32_t                    memoryBarrierCount,
    const VkMemoryBarrier*      pMemoryBarriers,
    uint32_t                    bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t                    imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers)
{
    // Metal 自动处理资源依赖，barrier 为空操作
    (void)commandBuffer; (void)srcStageMask; (void)dstStageMask; (void)dependencyFlags;
    (void)memoryBarrierCount; (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount; (void)pBufferMemoryBarriers;
    (void)imageMemoryBarrierCount; (void)pImageMemoryBarriers;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkCmdBlitImage(
    VkCommandBuffer   commandBuffer,
    VkImage           srcImage,
    VkImageLayout     srcImageLayout,
    VkImage           dstImage,
    VkImageLayout     dstImageLayout,
    uint32_t          regionCount,
    const VkImageBlit* pRegions,
    VkFilter          filter)
{
    (void)srcImageLayout; (void)dstImageLayout; (void)filter;
    auto* src = AS_MTL(Image, srcImage);
    auto* dst = AS_MTL(Image, dstImage);
    if (!src || !dst || !pRegions || regionCount == 0) return;

    auto* cb = AS_MTL(CommandBuffer, commandBuffer);
    if (!cb || !cb->commandBuffer) return;

    if (cb->encoder) {
        [cb->encoder endEncoding];
        cb->encoder = nil;
    }

    id<MTLBlitCommandEncoder> blit = [cb->commandBuffer blitCommandEncoder];
    for (uint32_t i = 0; i < regionCount; i++) {
        auto& r = pRegions[i];
        uint32_t srcW = std::abs(r.srcOffsets[1].x - r.srcOffsets[0].x);
        uint32_t srcH = std::abs(r.srcOffsets[1].y - r.srcOffsets[0].y);

        if (src->texture && dst->texture) {
            [blit copyFromTexture:src->texture
                      sourceSlice:0
                      sourceLevel:r.srcSubresource.mipLevel
                     sourceOrigin:MTLOriginMake(r.srcOffsets[0].x, r.srcOffsets[0].y, 0)
                       sourceSize:MTLSizeMake(srcW, srcH, 1)
                        toTexture:dst->texture
                 destinationSlice:0
                 destinationLevel:r.dstSubresource.mipLevel
                destinationOrigin:MTLOriginMake(r.dstOffsets[0].x, r.dstOffsets[0].y, 0)];
        }
    }
    [blit endEncoding];
}

// ============================================================================
// Synchronization
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkWaitForFences(
    VkDevice       device,
    uint32_t       fenceCount,
    const VkFence* pFences,
    VkBool32       waitAll,
    uint64_t       timeout)
{
    (void)device; (void)waitAll;
    if (!pFences) return VK_SUCCESS;

    for (uint32_t i = 0; i < fenceCount; i++) {
        auto* f = AS_MTL(Fence, pFences[i]);
        if (!f || !f->semaphore) continue;
        if (f->signaled) continue;  // 已被 completion handler 标记完成
        // 等待 GPU 完成（completion handler 会 signal semaphore）
        // Vulkan timeout 单位为纳秒，UINT64_MAX 表示永久等待
        dispatch_time_t deadline = (timeout >= (uint64_t)INT64_MAX)
            ? DISPATCH_TIME_FOREVER
            : dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout);
        long result = dispatch_semaphore_wait(f->semaphore, deadline);
        if (result != 0) return VK_TIMEOUT;
        f->signaled = true;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkResetFences(
    VkDevice       device,
    uint32_t       fenceCount,
    const VkFence* pFences)
{
    (void)device;
    if (pFences) {
        for (uint32_t i = 0; i < fenceCount; ++i) {
            auto* f = AS_MTL(Fence, pFences[i]);
            if (f) {
                f->signaled = false;
                // 重新创建 semaphore (重置为 0)
                f->semaphore = dispatch_semaphore_create(0);
            }
        }
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkQueueSubmit(
    VkQueue             queue,
    uint32_t            submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence             fence)
{
    (void)queue;

    if (pSubmits) {
        for (uint32_t s = 0; s < submitCount; s++) {
            // 检查是否有 wait semaphore（标志这是一次帧提交，而非离屏命令）
            bool isFrameSubmit = (pSubmits[s].waitSemaphoreCount > 0);

            for (uint32_t c = 0; c < pSubmits[s].commandBufferCount; c++) {
                auto* cb = AS_MTL(CommandBuffer, pSubmits[s].pCommandBuffers[c]);
                if (!cb || !cb->commandBuffer) continue;

                // 只在帧提交时 present swapchain drawable
                if (isFrameSubmit && s_currentSwapchain && s_currentSwapchain->currentDrawable) {
                    [cb->commandBuffer presentDrawable:s_currentSwapchain->currentDrawable];
                    s_currentSwapchain->currentDrawable = nil;
                }

                // 设置 fence 完成回调
                if (fence != VK_NULL_HANDLE) {
                    auto* f = AS_MTL(Fence, fence);
                    if (f && f->semaphore) {
                        dispatch_semaphore_t sem = f->semaphore;
                        MTL_Fence* fencePtr = f;
                        [cb->commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull) {
                            fencePtr->signaled = true;
                            dispatch_semaphore_signal(sem);
                        }];
                    }
                }

                s_lastCommittedBuffer = cb->commandBuffer;
                [cb->commandBuffer commit];
                cb->commandBuffer = nil;
            }
        }
    }

    // 注意：不在这里设置 signaled=true，由 completion handler 异步设置

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkQueueWaitIdle(VkQueue queue)
{
    (void)queue;
    // 等待最后提交的 command buffer 完成
    if (s_lastCommittedBuffer) {
        [s_lastCommittedBuffer waitUntilCompleted];
        s_lastCommittedBuffer = nil;
    }
    return VK_SUCCESS;
}

// ============================================================================
// Pipeline
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreatePipelineLayout(
    VkDevice                          device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*      pAllocator,
    VkPipelineLayout*                 pPipelineLayout)
{
    (void)device; (void)pAllocator;
    auto* layout = new MTL_PipelineLayout();
    if (pCreateInfo) {
        for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; ++i)
            layout->setLayouts.push_back(pCreateInfo->pSetLayouts[i]);
        for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; ++i) {
            layout->pushConstantRanges.push_back(pCreateInfo->pPushConstantRanges[i]);
            layout->pushConstSize += pCreateInfo->pPushConstantRanges[i].size;
        }
    }
    *pPipelineLayout = TO_VK(VkPipelineLayout, layout);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyPipelineLayout(
    VkDevice                     device,
    VkPipelineLayout             pipelineLayout,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_MTL(PipelineLayout, pipelineLayout);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateShaderModule(
    VkDevice                        device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*    pAllocator,
    VkShaderModule*                 pShaderModule)
{
    (void)device; (void)pAllocator;
    auto* mod = new MTL_ShaderModule();

    if (pCreateInfo && pCreateInfo->pCode && pCreateInfo->codeSize > 0) {
        const uint32_t* words = pCreateInfo->pCode;
        bool isSpirvData = (pCreateInfo->codeSize >= 4 && words[0] == SPIRV_MAGIC);

        if (isSpirvData) {
            // SPIRV-Cross 转换 SPIR-V → MSL（在独立 C++ 编译单元中执行，避免 ARC 干扰）
            size_t wordCount = pCreateInfo->codeSize / sizeof(uint32_t);
            std::vector<uint32_t> spirvData(words, words + wordCount);
            compileSpirvToMSL(std::move(spirvData), mod);
        } else {
            // MSL 文本
            std::string raw(reinterpret_cast<const char*>(pCreateInfo->pCode),
                            pCreateInfo->codeSize);
            while (!raw.empty() && raw.back() == '\0')
                raw.pop_back();
            mod->mslSource = raw;
        }
    }

    *pShaderModule = TO_VK(VkShaderModule, mod);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyShaderModule(
    VkDevice                     device,
    VkShaderModule               shaderModule,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_MTL(ShaderModule, shaderModule);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateGraphicsPipelines(
    VkDevice                            device,
    VkPipelineCache                     pipelineCache,
    uint32_t                            createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks*        pAllocator,
    VkPipeline*                         pPipelines)
{
    (void)pipelineCache; (void)pAllocator;
    auto* dev = AS_MTL(Device, device);
    if (!dev) return VK_ERROR_DEVICE_LOST;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        auto& ci = pCreateInfos[i];
        auto* pipe = new MTL_Pipeline();
        pipe->layout = ci.layout;

        // 所有 pipeline 统一路径: 每个 stage 的 MSL 由 SPIRV-Cross 生成
        id<MTLFunction> vertexFunc = nil;
        id<MTLFunction> fragmentFunc = nil;
        MTL_ShaderModule* vsMod = nullptr;
        MTL_ShaderModule* fsMod = nullptr;

        // 逐 stage 编译 MSL 并获取 MTLFunction
        for (uint32_t s = 0; s < ci.stageCount; s++) {
            auto* sm = AS_MTL(ShaderModule, ci.pStages[s].module);
            if (!sm || sm->mslSource.empty()) continue;

            NSString* mslSrc = [NSString stringWithUTF8String:sm->mslSource.c_str()];
            NSError* error = nil;
            id<MTLLibrary> lib = [dev->device newLibraryWithSource:mslSrc
                                                           options:nil
                                                             error:&error];
            if (!lib) {
                SDL_Log("[VkMetal] MSL compile FAILED (stage %u): %s", s,
                        error ? [[error localizedDescription] UTF8String] : "unknown");
                continue;
            }

            // 确定 entry point 名称:
            // 优先使用 SPIRV-Cross 记录的名称，否则用 Vulkan 传入的名称
            NSString* funcName = nil;
            if (!sm->entryPointName.empty()) {
                funcName = [NSString stringWithUTF8String:sm->entryPointName.c_str()];
            } else if (ci.pStages[s].pName) {
                funcName = [NSString stringWithUTF8String:ci.pStages[s].pName];
            }

            id<MTLFunction> func = funcName ? [lib newFunctionWithName:funcName] : nil;

            // 如果找不到，尝试获取 library 中唯一的函数
            if (!func) {
                NSArray<NSString*>* names = [lib functionNames];
                if (names.count > 0) {
                    func = [lib newFunctionWithName:names[0]];
                    SDL_Log("[VkMetal]   fallback: 使用 library 中的函数 '%s'",
                            [names[0] UTF8String]);
                }
            }

            if (ci.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT) {
                vertexFunc = func;
                vsMod = sm;
            } else if (ci.pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                fragmentFunc = func;
                fsMod = sm;
            }
        }

        if (!vertexFunc || !fragmentFunc) {
            SDL_Log("[VkMetal] WARNING: vertex/fragment function not found for pipeline %u", i);
            pPipelines[i] = TO_VK(VkPipeline, pipe);
            continue;
        }

        // 保存绑定映射到 pipeline，供 draw 时使用
        if (vsMod) pipe->vsBindings = vsMod->bindings;
        if (fsMod) pipe->fsBindings = fsMod->bindings;

        // 构建 MTLRenderPipelineDescriptor
        MTLRenderPipelineDescriptor* pipeDesc = [[MTLRenderPipelineDescriptor alloc] init];
        pipeDesc.vertexFunction = vertexFunc;
        pipeDesc.fragmentFunction = fragmentFunc;

        // 顶点描述符
        if (ci.pVertexInputState) {
            MTLVertexDescriptor* vertDesc = [[MTLVertexDescriptor alloc] init];

            // 确定 SPIRV-Cross 分配给顶点输入的 Metal buffer index
            uint32_t vbBaseIndex = 0;
            if (vsMod && vsMod->bindings.hasVertexInputs) {
                vbBaseIndex = vsMod->bindings.vertexBufferIndex;
            }

            for (uint32_t a = 0; a < ci.pVertexInputState->vertexAttributeDescriptionCount; a++) {
                auto& attr = ci.pVertexInputState->pVertexAttributeDescriptions[a];
                MTLVertexFormat mtlFmt = toMTLVertexFormat(attr.format);
                vertDesc.attributes[attr.location].format = mtlFmt;
                vertDesc.attributes[attr.location].offset = attr.offset;
                // Vulkan binding → Metal buffer index:
                // SPIRV-Cross 通常把 binding 0 映射到 vbBaseIndex，
                // binding 1 映射到 vbBaseIndex+1，以此类推
                vertDesc.attributes[attr.location].bufferIndex = vbBaseIndex + attr.binding;
            }

            for (uint32_t b = 0; b < ci.pVertexInputState->vertexBindingDescriptionCount; b++) {
                auto& binding = ci.pVertexInputState->pVertexBindingDescriptions[b];
                uint32_t mtlBufIdx = vbBaseIndex + binding.binding;
                vertDesc.layouts[mtlBufIdx].stride = binding.stride;
                vertDesc.layouts[mtlBufIdx].stepFunction =
                    (binding.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
                        ? MTLVertexStepFunctionPerInstance
                        : MTLVertexStepFunctionPerVertex;
                if (binding.binding < 8) {
                    pipe->vertexStrides[binding.binding] = binding.stride;
                    if (binding.binding + 1 > pipe->vertexBindingCount)
                        pipe->vertexBindingCount = binding.binding + 1;
                }
            }

            SDL_Log("[VkMetal]   vertex descriptor: %u attrs, %u bindings, vbBaseIndex=%u",
                    ci.pVertexInputState->vertexAttributeDescriptionCount,
                    ci.pVertexInputState->vertexBindingDescriptionCount,
                    vbBaseIndex);
            pipeDesc.vertexDescriptor = vertDesc;
        }

        // 颜色附件格式和混合状态
        if (ci.renderPass) {
            auto* rp = AS_MTL(RenderPass, ci.renderPass);
            if (rp) {
                uint32_t colorIdx = 0;
                for (auto& att : rp->attachments) {
                    if (isDepthFormat(att.format)) {
                        pipeDesc.depthAttachmentPixelFormat = toMTLPixelFormat(att.format);
                        if (att.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                            pipeDesc.stencilAttachmentPixelFormat = toMTLPixelFormat(att.format);
                        }
                    } else {
                        pipeDesc.colorAttachments[colorIdx].pixelFormat = toMTLPixelFormat(att.format);

                        // 混合状态
                        if (ci.pColorBlendState && colorIdx < ci.pColorBlendState->attachmentCount) {
                            auto& blend = ci.pColorBlendState->pAttachments[colorIdx];
                            pipeDesc.colorAttachments[colorIdx].blendingEnabled = blend.blendEnable ? YES : NO;
                            if (blend.blendEnable) {
                                // 源颜色混合因子
                                auto toBlendFactor = [](VkBlendFactor f) -> MTLBlendFactor {
                                    switch (f) {
                                    case VK_BLEND_FACTOR_ZERO:                return MTLBlendFactorZero;
                                    case VK_BLEND_FACTOR_ONE:                 return MTLBlendFactorOne;
                                    case VK_BLEND_FACTOR_SRC_COLOR:           return MTLBlendFactorSourceColor;
                                    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
                                    case VK_BLEND_FACTOR_SRC_ALPHA:           return MTLBlendFactorSourceAlpha;
                                    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
                                    case VK_BLEND_FACTOR_DST_COLOR:           return MTLBlendFactorDestinationColor;
                                    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
                                    case VK_BLEND_FACTOR_DST_ALPHA:           return MTLBlendFactorDestinationAlpha;
                                    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
                                    default:                                  return MTLBlendFactorOne;
                                    }
                                };
                                auto toBlendOp = [](VkBlendOp op) -> MTLBlendOperation {
                                    switch (op) {
                                    case VK_BLEND_OP_ADD:              return MTLBlendOperationAdd;
                                    case VK_BLEND_OP_SUBTRACT:         return MTLBlendOperationSubtract;
                                    case VK_BLEND_OP_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
                                    case VK_BLEND_OP_MIN:              return MTLBlendOperationMin;
                                    case VK_BLEND_OP_MAX:              return MTLBlendOperationMax;
                                    default:                           return MTLBlendOperationAdd;
                                    }
                                };

                                pipeDesc.colorAttachments[colorIdx].sourceRGBBlendFactor = toBlendFactor(blend.srcColorBlendFactor);
                                pipeDesc.colorAttachments[colorIdx].destinationRGBBlendFactor = toBlendFactor(blend.dstColorBlendFactor);
                                pipeDesc.colorAttachments[colorIdx].rgbBlendOperation = toBlendOp(blend.colorBlendOp);
                                pipeDesc.colorAttachments[colorIdx].sourceAlphaBlendFactor = toBlendFactor(blend.srcAlphaBlendFactor);
                                pipeDesc.colorAttachments[colorIdx].destinationAlphaBlendFactor = toBlendFactor(blend.dstAlphaBlendFactor);
                                pipeDesc.colorAttachments[colorIdx].alphaBlendOperation = toBlendOp(blend.alphaBlendOp);
                            }

                            // 写入掩码
                            MTLColorWriteMask mask = MTLColorWriteMaskNone;
                            if (blend.colorWriteMask & VK_COLOR_COMPONENT_R_BIT) mask |= MTLColorWriteMaskRed;
                            if (blend.colorWriteMask & VK_COLOR_COMPONENT_G_BIT) mask |= MTLColorWriteMaskGreen;
                            if (blend.colorWriteMask & VK_COLOR_COMPONENT_B_BIT) mask |= MTLColorWriteMaskBlue;
                            if (blend.colorWriteMask & VK_COLOR_COMPONENT_A_BIT) mask |= MTLColorWriteMaskAlpha;
                            pipeDesc.colorAttachments[colorIdx].writeMask = mask;
                        }
                        colorIdx++;
                    }
                }
            }
        }

        // 创建管线状态
        NSError* error = nil;
        pipe->pipelineState = [dev->device newRenderPipelineStateWithDescriptor:pipeDesc
                                                                         error:&error];
        if (!pipe->pipelineState) {
            SDL_Log("[VkMetal] Pipeline create FAILED: %s",
                    error ? [[error localizedDescription] UTF8String] : "unknown");
        }

        // 创建深度模板状态
        if (ci.pDepthStencilState) {
            MTLDepthStencilDescriptor* dsDesc = [[MTLDepthStencilDescriptor alloc] init];
            dsDesc.depthCompareFunction = ci.pDepthStencilState->depthTestEnable
                ? toMTLCompareFunc(ci.pDepthStencilState->depthCompareOp)
                : MTLCompareFunctionAlways;
            dsDesc.depthWriteEnabled = ci.pDepthStencilState->depthWriteEnable ? YES : NO;
            pipe->depthStencilState = [dev->device newDepthStencilStateWithDescriptor:dsDesc];
        }

        // 光栅化状态
        if (ci.pInputAssemblyState) {
            pipe->topology = (uint32_t)toMTLPrimitiveType(ci.pInputAssemblyState->topology);
        }

        if (ci.pRasterizationState) {
            auto* rs = ci.pRasterizationState;
            if (rs->cullMode == VK_CULL_MODE_NONE)
                pipe->cullMode = (uint32_t)MTLCullModeNone;
            else if (rs->cullMode == VK_CULL_MODE_FRONT_BIT)
                pipe->cullMode = (uint32_t)MTLCullModeFront;
            else
                pipe->cullMode = (uint32_t)MTLCullModeBack;

            // Vulkan CCW = front face → Metal Winding
            pipe->frontFace = (rs->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE)
                ? (uint32_t)MTLWindingCounterClockwise : (uint32_t)MTLWindingClockwise;

            pipe->depthBiasEnable = rs->depthBiasEnable;
            pipe->depthBiasConstant = rs->depthBiasConstantFactor;
            pipe->depthBiasSlope = rs->depthBiasSlopeFactor;
            pipe->depthBiasClamp = rs->depthBiasClamp;
        }

        pPipelines[i] = TO_VK(VkPipeline, pipe);
        SDL_Log("[VkMetal] Pipeline created (vs=%p fs=%p)", (void*)vsMod, (void*)fsMod);
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyPipeline(
    VkDevice                     device,
    VkPipeline                   pipeline,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!pipeline) return;
    auto* pipe = AS_MTL(Pipeline, pipeline);
    pipe->pipelineState = nil;
    pipe->depthStencilState = nil;
    delete pipe;
}

// ============================================================================
// Render Pass & Framebuffer
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateRenderPass(
    VkDevice                       device,
    const VkRenderPassCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks*   pAllocator,
    VkRenderPass*                  pRenderPass)
{
    (void)device; (void)pAllocator;
    auto* rp = new MTL_RenderPass();
    if (pCreateInfo) {
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
            rp->attachments.push_back(pCreateInfo->pAttachments[i]);
        rp->colorAttachmentCount = 0;
        rp->hasDepth = false;
        if (pCreateInfo->subpassCount > 0 && pCreateInfo->pSubpasses) {
            rp->colorAttachmentCount = pCreateInfo->pSubpasses[0].colorAttachmentCount;
            rp->hasDepth = (pCreateInfo->pSubpasses[0].pDepthStencilAttachment != nullptr);
        }
    }
    *pRenderPass = TO_VK(VkRenderPass, rp);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyRenderPass(
    VkDevice                     device,
    VkRenderPass                 renderPass,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_MTL(RenderPass, renderPass);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateFramebuffer(
    VkDevice                       device,
    const VkFramebufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*   pAllocator,
    VkFramebuffer*                 pFramebuffer)
{
    (void)device; (void)pAllocator;
    auto* fb = new MTL_Framebuffer();
    if (pCreateInfo) {
        fb->width = pCreateInfo->width;
        fb->height = pCreateInfo->height;
        fb->renderPass = pCreateInfo->renderPass;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
            fb->attachments.push_back(pCreateInfo->pAttachments[i]);
    }
    *pFramebuffer = TO_VK(VkFramebuffer, fb);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyFramebuffer(
    VkDevice                     device,
    VkFramebuffer                framebuffer,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_MTL(Framebuffer, framebuffer);
}

// ============================================================================
// Descriptor
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateDescriptorPool(
    VkDevice                          device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*      pAllocator,
    VkDescriptorPool*                 pDescriptorPool)
{
    (void)pCreateInfo; (void)pAllocator;
    auto* pool = new MTL_DescriptorPool();
    pool->device = device;
    *pDescriptorPool = TO_VK(VkDescriptorPool, pool);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyDescriptorPool(
    VkDevice                     device,
    VkDescriptorPool             descriptorPool,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_MTL(DescriptorPool, descriptorPool);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateDescriptorSetLayout(
    VkDevice                                device,
    const VkDescriptorSetLayoutCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks*            pAllocator,
    VkDescriptorSetLayout*                  pSetLayout)
{
    (void)device; (void)pAllocator;
    auto* layout = new MTL_DescriptorSetLayout();
    if (pCreateInfo) {
        for (uint32_t i = 0; i < pCreateInfo->bindingCount; ++i)
            layout->bindings.push_back(pCreateInfo->pBindings[i]);
    }
    *pSetLayout = TO_VK(VkDescriptorSetLayout, layout);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyDescriptorSetLayout(
    VkDevice                     device,
    VkDescriptorSetLayout        descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_MTL(DescriptorSetLayout, descriptorSetLayout);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkAllocateDescriptorSets(
    VkDevice                           device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet*                   pDescriptorSets)
{
    (void)device;
    if (pAllocateInfo && pDescriptorSets) {
        for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
            auto* ds = new MTL_DescriptorSet();
            ds->layout = pAllocateInfo->pSetLayouts ? pAllocateInfo->pSetLayouts[i] : VK_NULL_HANDLE;
            pDescriptorSets[i] = TO_VK(VkDescriptorSet, ds);
        }
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkFreeDescriptorSets(
    VkDevice               device,
    VkDescriptorPool       descriptorPool,
    uint32_t               descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets)
{
    (void)device; (void)descriptorPool;
    if (pDescriptorSets) {
        for (uint32_t i = 0; i < descriptorSetCount; ++i) {
            delete AS_MTL(DescriptorSet, pDescriptorSets[i]);
        }
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkUpdateDescriptorSets(
    VkDevice                    device,
    uint32_t                    descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t                    descriptorCopyCount,
    const VkCopyDescriptorSet*  pDescriptorCopies)
{
    (void)device; (void)descriptorCopyCount; (void)pDescriptorCopies;
    if (!pDescriptorWrites) return;

    for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
        const auto& write = pDescriptorWrites[i];
        auto* ds = AS_MTL(DescriptorSet, write.dstSet);
        if (!ds) continue;

        if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            if (write.pBufferInfo) {
                uint32_t slot = write.dstBinding;
                if (slot < 8) {
                    ds->uboVkBuffers[slot] = write.pBufferInfo->buffer;
                    ds->uboOffsets[slot] = write.pBufferInfo->offset;
                    if (slot >= ds->uboCount)
                        ds->uboCount = slot + 1;
                }
            }
        } else if (write.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                   write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            if (write.pImageInfo) {
                for (uint32_t d = 0; d < write.descriptorCount; d++) {
                    uint32_t slot = write.dstBinding + d;
                    if (slot >= 8) break;
                    auto& imgInfo = write.pImageInfo[d];
                    ds->imageViews[slot] = imgInfo.imageView;
                    ds->samplers[slot] = imgInfo.sampler;
                    if (slot >= ds->textureCount)
                        ds->textureCount = slot + 1;
                }
            }
        }
    }
}

// ============================================================================
// Sync Objects
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateSemaphore(
    VkDevice                     device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSemaphore*                 pSemaphore)
{
    (void)device; (void)pCreateInfo; (void)pAllocator;
    auto* sem = new MTL_Semaphore();
    *pSemaphore = TO_VK(VkSemaphore, sem);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroySemaphore(
    VkDevice                     device,
    VkSemaphore                  semaphore,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_MTL(Semaphore, semaphore);
}

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateFence(
    VkDevice                 device,
    const VkFenceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFence*                 pFence)
{
    (void)device; (void)pAllocator;
    auto* fence = new MTL_Fence();
    fence->semaphore = dispatch_semaphore_create(0);
    if (pCreateInfo && (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT)) {
        fence->signaled = true;
        dispatch_semaphore_signal(fence->semaphore);
    }
    *pFence = TO_VK(VkFence, fence);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyFence(
    VkDevice                     device,
    VkFence                      fence,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!fence) return;
    auto* f = AS_MTL(Fence, fence);
    // dispatch_semaphore_t 由 ARC 管理
    delete f;
}

// ============================================================================
// Debug
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL mtl_vkCreateDebugUtilsMessengerEXT(
    VkInstance                                  instance,
    const VkDebugUtilsMessengerCreateInfoEXT*   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDebugUtilsMessengerEXT*                   pMessenger)
{
    (void)instance; (void)pAllocator;
    auto* dbg = new MTL_DebugMessenger();
    if (pCreateInfo) {
        dbg->callback = pCreateInfo->pfnUserCallback;
        dbg->userData = pCreateInfo->pUserData;
    }
    *pMessenger = TO_VK(VkDebugUtilsMessengerEXT, dbg);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mtl_vkDestroyDebugUtilsMessengerEXT(
    VkInstance                   instance,
    VkDebugUtilsMessengerEXT     messenger,
    const VkAllocationCallbacks* pAllocator)
{
    (void)instance; (void)pAllocator;
    delete AS_MTL(DebugMessenger, messenger);
}

// ============================================================================
// vkLoadMetalDispatch — 注册所有 mtl_vkXxx 到全局函数指针
// ============================================================================

void vkLoadMetalDispatch()
{
    #define VK_MTL(fn) fn = mtl_##fn

    // Instance
    VK_MTL(vkCreateInstance);
    VK_MTL(vkDestroyInstance);
    VK_MTL(vkEnumerateInstanceExtensionProperties);
    VK_MTL(vkEnumerateInstanceLayerProperties);
    VK_MTL(vkGetInstanceProcAddr);

    // Physical Device
    VK_MTL(vkEnumeratePhysicalDevices);
    VK_MTL(vkGetPhysicalDeviceProperties);
    VK_MTL(vkGetPhysicalDeviceFeatures);
    VK_MTL(vkGetPhysicalDeviceFeatures2);
    VK_MTL(vkGetPhysicalDeviceMemoryProperties);
    VK_MTL(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_MTL(vkEnumerateDeviceExtensionProperties);

    // Device & Queue
    VK_MTL(vkCreateDevice);
    VK_MTL(vkDestroyDevice);
    VK_MTL(vkGetDeviceProcAddr);
    VK_MTL(vkGetDeviceQueue);
    VK_MTL(vkDeviceWaitIdle);

    // Surface
    VK_MTL(vkDestroySurfaceKHR);
    VK_MTL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VK_MTL(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VK_MTL(vkGetPhysicalDeviceSurfacePresentModesKHR);
    VK_MTL(vkGetPhysicalDeviceSurfaceSupportKHR);

    // Swapchain
    VK_MTL(vkCreateSwapchainKHR);
    VK_MTL(vkDestroySwapchainKHR);
    VK_MTL(vkGetSwapchainImagesKHR);
    VK_MTL(vkAcquireNextImageKHR);
    VK_MTL(vkQueuePresentKHR);

    // Buffer
    VK_MTL(vkCreateBuffer);
    VK_MTL(vkDestroyBuffer);
    VK_MTL(vkGetBufferMemoryRequirements);

    // Memory
    VK_MTL(vkAllocateMemory);
    VK_MTL(vkFreeMemory);
    VK_MTL(vkBindBufferMemory);
    VK_MTL(vkMapMemory);
    VK_MTL(vkUnmapMemory);
    VK_MTL(vkFlushMappedMemoryRanges);

    // Image
    VK_MTL(vkCreateImage);
    VK_MTL(vkDestroyImage);
    VK_MTL(vkGetImageMemoryRequirements);
    VK_MTL(vkBindImageMemory);
    VK_MTL(vkCreateImageView);
    VK_MTL(vkDestroyImageView);

    // Sampler
    VK_MTL(vkCreateSampler);
    VK_MTL(vkDestroySampler);

    // Command Pool & Buffer
    VK_MTL(vkCreateCommandPool);
    VK_MTL(vkDestroyCommandPool);
    VK_MTL(vkResetCommandPool);
    VK_MTL(vkAllocateCommandBuffers);
    VK_MTL(vkFreeCommandBuffers);
    VK_MTL(vkBeginCommandBuffer);
    VK_MTL(vkEndCommandBuffer);
    VK_MTL(vkResetCommandBuffer);

    // Command Recording
    VK_MTL(vkCmdBeginRenderPass);
    VK_MTL(vkCmdEndRenderPass);
    VK_MTL(vkCmdBindPipeline);
    VK_MTL(vkCmdBindDescriptorSets);
    VK_MTL(vkCmdBindVertexBuffers);
    VK_MTL(vkCmdBindIndexBuffer);
    VK_MTL(vkCmdDraw);
    VK_MTL(vkCmdDrawIndexed);
    VK_MTL(vkCmdSetViewport);
    VK_MTL(vkCmdSetScissor);
    VK_MTL(vkCmdPushConstants);
    VK_MTL(vkCmdCopyBuffer);
    VK_MTL(vkCmdCopyBufferToImage);
    VK_MTL(vkCmdCopyImageToBuffer);
    VK_MTL(vkCmdPipelineBarrier);
    VK_MTL(vkCmdBlitImage);

    // Synchronization
    VK_MTL(vkWaitForFences);
    VK_MTL(vkResetFences);
    VK_MTL(vkQueueSubmit);
    VK_MTL(vkQueueWaitIdle);

    // Pipeline
    VK_MTL(vkCreatePipelineLayout);
    VK_MTL(vkDestroyPipelineLayout);
    VK_MTL(vkCreateShaderModule);
    VK_MTL(vkDestroyShaderModule);
    VK_MTL(vkCreateGraphicsPipelines);
    VK_MTL(vkDestroyPipeline);

    // Render Pass & Framebuffer
    VK_MTL(vkCreateRenderPass);
    VK_MTL(vkDestroyRenderPass);
    VK_MTL(vkCreateFramebuffer);
    VK_MTL(vkDestroyFramebuffer);

    // Descriptor
    VK_MTL(vkCreateDescriptorPool);
    VK_MTL(vkDestroyDescriptorPool);
    VK_MTL(vkCreateDescriptorSetLayout);
    VK_MTL(vkDestroyDescriptorSetLayout);
    VK_MTL(vkAllocateDescriptorSets);
    VK_MTL(vkFreeDescriptorSets);
    VK_MTL(vkUpdateDescriptorSets);

    // Sync Objects
    VK_MTL(vkCreateSemaphore);
    VK_MTL(vkDestroySemaphore);
    VK_MTL(vkCreateFence);
    VK_MTL(vkDestroyFence);

    // Debug
    VK_MTL(vkCreateDebugUtilsMessengerEXT);
    VK_MTL(vkDestroyDebugUtilsMessengerEXT);

    #undef VK_MTL
}
