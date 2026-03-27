#include "renderer/VkDispatch.h"
#include <iostream>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>  // Android/Linux: dlopen, dlsym
#include <SDL.h>
#include <SDL_vulkan.h>
#endif

// ============================================================================
// 全局函数指针定义 (初始为 nullptr)
// ============================================================================

// --- Instance ---
PFN_vkCreateInstance                         vkCreateInstance = nullptr;
PFN_vkDestroyInstance                        vkDestroyInstance = nullptr;
PFN_vkEnumerateInstanceExtensionProperties   vkEnumerateInstanceExtensionProperties = nullptr;
PFN_vkEnumerateInstanceLayerProperties       vkEnumerateInstanceLayerProperties = nullptr;
PFN_vkGetInstanceProcAddr                    vkGetInstanceProcAddr = nullptr;

// --- Physical Device ---
PFN_vkEnumeratePhysicalDevices               vkEnumeratePhysicalDevices = nullptr;
PFN_vkGetPhysicalDeviceProperties            vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceFeatures              vkGetPhysicalDeviceFeatures = nullptr;
PFN_vkGetPhysicalDeviceFeatures2             vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties      vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkEnumerateDeviceExtensionProperties     vkEnumerateDeviceExtensionProperties = nullptr;

// --- Device & Queue ---
PFN_vkCreateDevice                           vkCreateDevice = nullptr;
PFN_vkDestroyDevice                          vkDestroyDevice = nullptr;
PFN_vkGetDeviceProcAddr                      vkGetDeviceProcAddr = nullptr;
PFN_vkGetDeviceQueue                         vkGetDeviceQueue = nullptr;
PFN_vkDeviceWaitIdle                         vkDeviceWaitIdle = nullptr;

// --- Surface ---
PFN_vkDestroySurfaceKHR                      vkDestroySurfaceKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR     vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR     vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
#ifdef VK_USE_PLATFORM_WIN32_KHR
PFN_vkCreateWin32SurfaceKHR                  vkCreateWin32SurfaceKHR = nullptr;
#endif

// --- Swapchain ---
PFN_vkCreateSwapchainKHR                     vkCreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR                    vkDestroySwapchainKHR = nullptr;
PFN_vkGetSwapchainImagesKHR                  vkGetSwapchainImagesKHR = nullptr;
PFN_vkAcquireNextImageKHR                    vkAcquireNextImageKHR = nullptr;
PFN_vkQueuePresentKHR                        vkQueuePresentKHR = nullptr;

// --- Buffer ---
PFN_vkCreateBuffer                           vkCreateBuffer = nullptr;
PFN_vkDestroyBuffer                          vkDestroyBuffer = nullptr;
PFN_vkGetBufferMemoryRequirements            vkGetBufferMemoryRequirements = nullptr;

// --- Memory ---
PFN_vkAllocateMemory                         vkAllocateMemory = nullptr;
PFN_vkFreeMemory                             vkFreeMemory = nullptr;
PFN_vkBindBufferMemory                       vkBindBufferMemory = nullptr;
PFN_vkMapMemory                              vkMapMemory = nullptr;
PFN_vkUnmapMemory                            vkUnmapMemory = nullptr;
PFN_vkFlushMappedMemoryRanges                vkFlushMappedMemoryRanges = nullptr;

// --- Image ---
PFN_vkCreateImage                            vkCreateImage = nullptr;
PFN_vkDestroyImage                           vkDestroyImage = nullptr;
PFN_vkGetImageMemoryRequirements             vkGetImageMemoryRequirements = nullptr;
PFN_vkBindImageMemory                        vkBindImageMemory = nullptr;
PFN_vkCreateImageView                        vkCreateImageView = nullptr;
PFN_vkDestroyImageView                       vkDestroyImageView = nullptr;

// --- Sampler ---
PFN_vkCreateSampler                          vkCreateSampler = nullptr;
PFN_vkDestroySampler                         vkDestroySampler = nullptr;

// --- Command Pool & Buffer ---
PFN_vkCreateCommandPool                      vkCreateCommandPool = nullptr;
PFN_vkDestroyCommandPool                     vkDestroyCommandPool = nullptr;
PFN_vkResetCommandPool                       vkResetCommandPool = nullptr;
PFN_vkAllocateCommandBuffers                 vkAllocateCommandBuffers = nullptr;
PFN_vkFreeCommandBuffers                     vkFreeCommandBuffers = nullptr;
PFN_vkBeginCommandBuffer                     vkBeginCommandBuffer = nullptr;
PFN_vkEndCommandBuffer                       vkEndCommandBuffer = nullptr;
PFN_vkResetCommandBuffer                     vkResetCommandBuffer = nullptr;

// --- Command Recording ---
PFN_vkCmdBeginRenderPass                     vkCmdBeginRenderPass = nullptr;
PFN_vkCmdEndRenderPass                       vkCmdEndRenderPass = nullptr;
PFN_vkCmdBindPipeline                        vkCmdBindPipeline = nullptr;
PFN_vkCmdBindDescriptorSets                  vkCmdBindDescriptorSets = nullptr;
PFN_vkCmdBindVertexBuffers                   vkCmdBindVertexBuffers = nullptr;
PFN_vkCmdBindIndexBuffer                     vkCmdBindIndexBuffer = nullptr;
PFN_vkCmdDraw                                vkCmdDraw = nullptr;
PFN_vkCmdDrawIndexed                         vkCmdDrawIndexed = nullptr;
PFN_vkCmdSetViewport                         vkCmdSetViewport = nullptr;
PFN_vkCmdSetScissor                          vkCmdSetScissor = nullptr;
PFN_vkCmdPushConstants                       vkCmdPushConstants = nullptr;
PFN_vkCmdCopyBuffer                          vkCmdCopyBuffer = nullptr;
PFN_vkCmdCopyBufferToImage                   vkCmdCopyBufferToImage = nullptr;
PFN_vkCmdCopyImageToBuffer                   vkCmdCopyImageToBuffer = nullptr;
PFN_vkCmdPipelineBarrier                     vkCmdPipelineBarrier = nullptr;
PFN_vkCmdBlitImage                           vkCmdBlitImage = nullptr;

// --- Synchronization ---
PFN_vkWaitForFences                          vkWaitForFences = nullptr;
PFN_vkResetFences                            vkResetFences = nullptr;
PFN_vkQueueSubmit                            vkQueueSubmit = nullptr;
PFN_vkQueueWaitIdle                          vkQueueWaitIdle = nullptr;

// --- Pipeline ---
PFN_vkCreatePipelineLayout                   vkCreatePipelineLayout = nullptr;
PFN_vkDestroyPipelineLayout                  vkDestroyPipelineLayout = nullptr;
PFN_vkCreateShaderModule                     vkCreateShaderModule = nullptr;
PFN_vkDestroyShaderModule                    vkDestroyShaderModule = nullptr;
PFN_vkCreateGraphicsPipelines                vkCreateGraphicsPipelines = nullptr;
PFN_vkDestroyPipeline                        vkDestroyPipeline = nullptr;

// --- Render Pass & Framebuffer ---
PFN_vkCreateRenderPass                       vkCreateRenderPass = nullptr;
PFN_vkDestroyRenderPass                      vkDestroyRenderPass = nullptr;
PFN_vkCreateFramebuffer                      vkCreateFramebuffer = nullptr;
PFN_vkDestroyFramebuffer                     vkDestroyFramebuffer = nullptr;

// --- Descriptor ---
PFN_vkCreateDescriptorPool                   vkCreateDescriptorPool = nullptr;
PFN_vkDestroyDescriptorPool                  vkDestroyDescriptorPool = nullptr;
PFN_vkCreateDescriptorSetLayout              vkCreateDescriptorSetLayout = nullptr;
PFN_vkDestroyDescriptorSetLayout             vkDestroyDescriptorSetLayout = nullptr;
PFN_vkAllocateDescriptorSets                 vkAllocateDescriptorSets = nullptr;
PFN_vkFreeDescriptorSets                     vkFreeDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets                   vkUpdateDescriptorSets = nullptr;

// --- Sync Objects ---
PFN_vkCreateSemaphore                        vkCreateSemaphore = nullptr;
PFN_vkDestroySemaphore                       vkDestroySemaphore = nullptr;
PFN_vkCreateFence                            vkCreateFence = nullptr;
PFN_vkDestroyFence                           vkDestroyFence = nullptr;

// --- Debug ---
PFN_vkCreateDebugUtilsMessengerEXT           vkCreateDebugUtilsMessengerEXT = nullptr;
PFN_vkDestroyDebugUtilsMessengerEXT          vkDestroyDebugUtilsMessengerEXT = nullptr;

// ============================================================================
// 加载实现
// ============================================================================

namespace {

#ifdef _WIN32
static HMODULE s_vulkanLib = nullptr;
#endif
static int s_backend = 0; // 0=Vulkan, 1=D3D12, 2=D3D11, 3=OpenGL, 4=GLES
static bool s_hasClipControl = false;

} // anonymous namespace

namespace QymEngine {

#ifdef _WIN32

// 从 vulkan-1.dll 动态加载所有函数指针
static void loadFromVulkanDll()
{
    s_vulkanLib = LoadLibraryA("vulkan-1.dll");
    if (!s_vulkanLib) {
        throw std::runtime_error("[VkDispatch] Failed to load vulkan-1.dll");
    }

    auto load = [](const char* name) -> PFN_vkVoidFunction {
        return reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(s_vulkanLib, name));
    };

    // 先加载 vkGetInstanceProcAddr，后续可用它加载其余函数
    // 但为简单起见，直接从 DLL 导出表加载所有全局函数
    #define VK_LOAD(fn) fn = reinterpret_cast<PFN_##fn>(load(#fn))

    // Instance
    VK_LOAD(vkCreateInstance);
    VK_LOAD(vkDestroyInstance);
    VK_LOAD(vkEnumerateInstanceExtensionProperties);
    VK_LOAD(vkEnumerateInstanceLayerProperties);
    VK_LOAD(vkGetInstanceProcAddr);

    // Physical Device
    VK_LOAD(vkEnumeratePhysicalDevices);
    VK_LOAD(vkGetPhysicalDeviceProperties);
    VK_LOAD(vkGetPhysicalDeviceFeatures);
    VK_LOAD(vkGetPhysicalDeviceFeatures2);
    VK_LOAD(vkGetPhysicalDeviceMemoryProperties);
    VK_LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_LOAD(vkEnumerateDeviceExtensionProperties);

    // Device & Queue
    VK_LOAD(vkCreateDevice);
    VK_LOAD(vkDestroyDevice);
    VK_LOAD(vkGetDeviceProcAddr);
    VK_LOAD(vkGetDeviceQueue);
    VK_LOAD(vkDeviceWaitIdle);

    // Surface
    VK_LOAD(vkDestroySurfaceKHR);
    VK_LOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VK_LOAD(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VK_LOAD(vkGetPhysicalDeviceSurfacePresentModesKHR);
    VK_LOAD(vkGetPhysicalDeviceSurfaceSupportKHR);
    VK_LOAD(vkCreateWin32SurfaceKHR);

    // Swapchain
    VK_LOAD(vkCreateSwapchainKHR);
    VK_LOAD(vkDestroySwapchainKHR);
    VK_LOAD(vkGetSwapchainImagesKHR);
    VK_LOAD(vkAcquireNextImageKHR);
    VK_LOAD(vkQueuePresentKHR);

    // Buffer
    VK_LOAD(vkCreateBuffer);
    VK_LOAD(vkDestroyBuffer);
    VK_LOAD(vkGetBufferMemoryRequirements);

    // Memory
    VK_LOAD(vkAllocateMemory);
    VK_LOAD(vkFreeMemory);
    VK_LOAD(vkBindBufferMemory);
    VK_LOAD(vkMapMemory);
    VK_LOAD(vkUnmapMemory);
    VK_LOAD(vkFlushMappedMemoryRanges);

    // Image
    VK_LOAD(vkCreateImage);
    VK_LOAD(vkDestroyImage);
    VK_LOAD(vkGetImageMemoryRequirements);
    VK_LOAD(vkBindImageMemory);
    VK_LOAD(vkCreateImageView);
    VK_LOAD(vkDestroyImageView);

    // Sampler
    VK_LOAD(vkCreateSampler);
    VK_LOAD(vkDestroySampler);

    // Command Pool & Buffer
    VK_LOAD(vkCreateCommandPool);
    VK_LOAD(vkDestroyCommandPool);
    VK_LOAD(vkResetCommandPool);
    VK_LOAD(vkAllocateCommandBuffers);
    VK_LOAD(vkFreeCommandBuffers);
    VK_LOAD(vkBeginCommandBuffer);
    VK_LOAD(vkEndCommandBuffer);
    VK_LOAD(vkResetCommandBuffer);

    // Command Recording
    VK_LOAD(vkCmdBeginRenderPass);
    VK_LOAD(vkCmdEndRenderPass);
    VK_LOAD(vkCmdBindPipeline);
    VK_LOAD(vkCmdBindDescriptorSets);
    VK_LOAD(vkCmdBindVertexBuffers);
    VK_LOAD(vkCmdBindIndexBuffer);
    VK_LOAD(vkCmdDraw);
    VK_LOAD(vkCmdDrawIndexed);
    VK_LOAD(vkCmdSetViewport);
    VK_LOAD(vkCmdSetScissor);
    VK_LOAD(vkCmdPushConstants);
    VK_LOAD(vkCmdCopyBuffer);
    VK_LOAD(vkCmdCopyBufferToImage);
    VK_LOAD(vkCmdCopyImageToBuffer);
    VK_LOAD(vkCmdPipelineBarrier);
    VK_LOAD(vkCmdBlitImage);

    // Synchronization
    VK_LOAD(vkWaitForFences);
    VK_LOAD(vkResetFences);
    VK_LOAD(vkQueueSubmit);
    VK_LOAD(vkQueueWaitIdle);

    // Pipeline
    VK_LOAD(vkCreatePipelineLayout);
    VK_LOAD(vkDestroyPipelineLayout);
    VK_LOAD(vkCreateShaderModule);
    VK_LOAD(vkDestroyShaderModule);
    VK_LOAD(vkCreateGraphicsPipelines);
    VK_LOAD(vkDestroyPipeline);

    // Render Pass & Framebuffer
    VK_LOAD(vkCreateRenderPass);
    VK_LOAD(vkDestroyRenderPass);
    VK_LOAD(vkCreateFramebuffer);
    VK_LOAD(vkDestroyFramebuffer);

    // Descriptor
    VK_LOAD(vkCreateDescriptorPool);
    VK_LOAD(vkDestroyDescriptorPool);
    VK_LOAD(vkCreateDescriptorSetLayout);
    VK_LOAD(vkDestroyDescriptorSetLayout);
    VK_LOAD(vkAllocateDescriptorSets);
    VK_LOAD(vkFreeDescriptorSets);
    VK_LOAD(vkUpdateDescriptorSets);

    // Sync Objects
    VK_LOAD(vkCreateSemaphore);
    VK_LOAD(vkDestroySemaphore);
    VK_LOAD(vkCreateFence);
    VK_LOAD(vkDestroyFence);

    // Debug (这些可能不在 vulkan-1.dll 中，通过 vkGetInstanceProcAddr 加载)
    // 先尝试直接加载，失败的话后续通过 instance 加载
    VK_LOAD(vkCreateDebugUtilsMessengerEXT);
    VK_LOAD(vkDestroyDebugUtilsMessengerEXT);

    #undef VK_LOAD

    std::cout << "[VkDispatch] Loaded from vulkan-1.dll" << std::endl;
}

#else // !_WIN32 (Android/Linux)

static void* s_vulkanSo = nullptr;

// 从 libvulkan.so 动态加载函数指针 (Android/Linux)
// 使用 SDL_Vulkan_GetVkGetInstanceProcAddr 获取入口点（SDL 已正确处理 Android 平台）
static void loadFromVulkanSo()
{
    // SDL 内部会加载 libvulkan 并提供 vkGetInstanceProcAddr
    auto rawGetProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
#ifndef __ANDROID__
    // Android 不静态链接 libvulkan，RTLD_DEFAULT 可能返回无效地址
    if (!rawGetProcAddr) {
        // 若已显式链接 Vulkan loader，优先直接取当前进程里的导出符号
        rawGetProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
    }
#endif
    if (!rawGetProcAddr) {
        // Fallback: 手动 dlopen
#ifdef __APPLE__
        // macOS: 先尝试 Homebrew MoltenVK 路径，再尝试标准名
        const char* vulkanLibNames[] = {
            "libvulkan.dylib",
            "libvulkan.1.dylib",
            "libMoltenVK.dylib",
            nullptr
        };
        for (const char** name = vulkanLibNames; *name && !s_vulkanSo; ++name) {
            s_vulkanSo = dlopen(*name, RTLD_NOW | RTLD_LOCAL);
        }
#else
        s_vulkanSo = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
        if (s_vulkanSo) {
            rawGetProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                dlsym(s_vulkanSo, "vkGetInstanceProcAddr"));
        }
    }
    if (!rawGetProcAddr) {
        throw std::runtime_error("[VkDispatch] Failed to get vkGetInstanceProcAddr");
    }
    vkGetInstanceProcAddr = rawGetProcAddr;

    // 阶段 1: 通过 vkGetInstanceProcAddr(NULL, ...) 只能加载全局函数
    // 实例级和设备级函数需要在 instance 创建后通过 vkPostInstanceInit 加载
    auto load = [&](const char* name) -> PFN_vkVoidFunction {
        return rawGetProcAddr(VK_NULL_HANDLE, name);
    };

    #define VK_LOAD(fn) fn = reinterpret_cast<PFN_##fn>(load(#fn))

    // 全局函数 (可以在 instance 创建前加载)
    VK_LOAD(vkCreateInstance);
    VK_LOAD(vkEnumerateInstanceExtensionProperties);
    VK_LOAD(vkEnumerateInstanceLayerProperties);

    #undef VK_LOAD

    std::cout << "[VkDispatch] Loaded from libvulkan.so (pre-instance)" << std::endl;
}

#endif // _WIN32

} // namespace QymEngine (暂时关闭)

// D3D12 后端加载（全局作用域，定义在 VkD3D12.cpp 中）
extern void vkLoadD3D12Dispatch();

// D3D11 后端加载 (D11_ 前缀结构体，与 D3D12 无 ODR 冲突)
extern void vkLoadD3D11Dispatch();

// OpenGL 后端加载 (GL_ 前缀结构体，iOS 上不可用)
#if !TARGET_OS_IOS
extern void vkLoadOpenGLDispatch();
#endif

#ifdef __APPLE__
// Metal 后端加载 (macOS 专用)
extern void vkLoadMetalDispatch();
#endif

namespace QymEngine { // 重新打开

void vkInitDispatch(int backendType)
{
    s_backend = backendType;

#ifdef _WIN32
    if (backendType == 1) {
        vkLoadD3D12Dispatch();
        std::cout << "[VkDispatch] Loaded D3D12 backend" << std::endl;
    } else if (backendType == 2) {
        vkLoadD3D11Dispatch();
        std::cout << "[VkDispatch] Loaded D3D11 backend" << std::endl;
    } else if (backendType == 3) {
        vkLoadOpenGLDispatch();
        std::cout << "[VkDispatch] Loaded OpenGL backend" << std::endl;
    } else if (backendType == 4) {
        vkLoadOpenGLDispatch();
        std::cout << "[VkDispatch] Loaded GLES backend" << std::endl;
    } else {
        loadFromVulkanDll();
    }
#else
    // 非 Windows 平台
#if !TARGET_OS_IOS
    if (backendType == 3 || backendType == 4) {
        vkLoadOpenGLDispatch();
        std::cout << "[VkDispatch] Loaded " << (backendType == 4 ? "GLES" : "OpenGL") << " backend" << std::endl;
    } else
#endif
    if (backendType == 5) {
#ifdef __APPLE__
        vkLoadMetalDispatch();
        std::cout << "[VkDispatch] Loaded Metal backend" << std::endl;
#else
        throw std::runtime_error("[VkDispatch] Metal backend is macOS-only");
#endif
    } else if (backendType == 0) {
        loadFromVulkanSo();
    } else {
        throw std::runtime_error("[VkDispatch] DirectX backends are Windows-only");
    }
#endif
}

void vkShutdownDispatch()
{
#ifdef _WIN32
    if (s_vulkanLib) {
        FreeLibrary(s_vulkanLib);
        s_vulkanLib = nullptr;
    }
#endif
    s_backend = 0;
    s_hasClipControl = false;
}

bool vkIsD3D12Backend()
{
    return s_backend == 1;
}

bool vkIsD3D11Backend()
{
    return s_backend == 2;
}

bool vkIsDirectXBackend()
{
    return s_backend == 1 || s_backend == 2;
}

bool vkIsOpenGLBackend()
{
    return s_backend == 3;
}

bool vkIsGLESBackend()
{
    return s_backend == 4;
}

bool vkIsMetalBackend()
{
    return s_backend == 5;
}

bool vkHasClipControl()
{
    return s_hasClipControl;
}

void vkSetClipControlSupport(bool enabled)
{
    s_hasClipControl = enabled;
}

bool vkGraphicsNeedFlipY()
{
    // DX 后端的 NDC Y 方向与 Vulkan 相反，全屏 quad 渲染需要翻转 UV Y
    return vkIsDirectXBackend();
}

void vkPostInstanceInit(VkInstance instance)
{
    // 非 Vulkan 后端不需要 post-instance 加载
    if (s_backend != 0) return;

#ifndef _WIN32
    // Android/Linux: 通过 vkGetInstanceProcAddr(instance, ...) 加载实例级函数
    if (!vkGetInstanceProcAddr || !instance) return;

    #define VK_ILOAD(fn) do { \
        fn = reinterpret_cast<PFN_##fn>(vkGetInstanceProcAddr(instance, #fn)); \
        if (!fn) SDL_Log("[VkDispatch] WARNING: %s is null", #fn); \
    } while(0)

    VK_ILOAD(vkDestroyInstance);
    VK_ILOAD(vkEnumeratePhysicalDevices);
    VK_ILOAD(vkGetPhysicalDeviceProperties);
    VK_ILOAD(vkGetPhysicalDeviceFeatures);
    VK_ILOAD(vkGetPhysicalDeviceFeatures2);
    VK_ILOAD(vkGetPhysicalDeviceMemoryProperties);
    VK_ILOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_ILOAD(vkEnumerateDeviceExtensionProperties);
    VK_ILOAD(vkCreateDevice);
    VK_ILOAD(vkDestroyDevice);
    VK_ILOAD(vkGetDeviceProcAddr);
    VK_ILOAD(vkGetDeviceQueue);
    VK_ILOAD(vkDeviceWaitIdle);
    VK_ILOAD(vkDestroySurfaceKHR);
    VK_ILOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VK_ILOAD(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VK_ILOAD(vkGetPhysicalDeviceSurfacePresentModesKHR);
    VK_ILOAD(vkGetPhysicalDeviceSurfaceSupportKHR);
    VK_ILOAD(vkCreateSwapchainKHR);
    VK_ILOAD(vkDestroySwapchainKHR);
    VK_ILOAD(vkGetSwapchainImagesKHR);
    VK_ILOAD(vkAcquireNextImageKHR);
    VK_ILOAD(vkQueuePresentKHR);
    VK_ILOAD(vkQueueSubmit);
    VK_ILOAD(vkQueueWaitIdle);
    VK_ILOAD(vkAllocateMemory);
    VK_ILOAD(vkFreeMemory);
    VK_ILOAD(vkMapMemory);
    VK_ILOAD(vkUnmapMemory);
    VK_ILOAD(vkFlushMappedMemoryRanges);
    VK_ILOAD(vkCreateBuffer);
    VK_ILOAD(vkDestroyBuffer);
    VK_ILOAD(vkGetBufferMemoryRequirements);
    VK_ILOAD(vkBindBufferMemory);
    VK_ILOAD(vkCreateImage);
    VK_ILOAD(vkDestroyImage);
    VK_ILOAD(vkGetImageMemoryRequirements);
    VK_ILOAD(vkBindImageMemory);
    VK_ILOAD(vkCreateImageView);
    VK_ILOAD(vkDestroyImageView);
    VK_ILOAD(vkCreateSampler);
    VK_ILOAD(vkDestroySampler);
    VK_ILOAD(vkCreateShaderModule);
    VK_ILOAD(vkDestroyShaderModule);
    VK_ILOAD(vkCreatePipelineLayout);
    VK_ILOAD(vkDestroyPipelineLayout);
    VK_ILOAD(vkCreateGraphicsPipelines);
    VK_ILOAD(vkDestroyPipeline);
    VK_ILOAD(vkCreateCommandPool);
    VK_ILOAD(vkDestroyCommandPool);
    VK_ILOAD(vkAllocateCommandBuffers);
    VK_ILOAD(vkFreeCommandBuffers);
    VK_ILOAD(vkBeginCommandBuffer);
    VK_ILOAD(vkEndCommandBuffer);
    VK_ILOAD(vkResetCommandBuffer);
    VK_ILOAD(vkCmdBeginRenderPass);
    VK_ILOAD(vkCmdEndRenderPass);
    VK_ILOAD(vkCmdBindPipeline);
    VK_ILOAD(vkCmdBindVertexBuffers);
    VK_ILOAD(vkCmdBindIndexBuffer);
    VK_ILOAD(vkCmdBindDescriptorSets);
    VK_ILOAD(vkCmdPushConstants);
    VK_ILOAD(vkCmdDraw);
    VK_ILOAD(vkCmdDrawIndexed);
    VK_ILOAD(vkCmdSetViewport);
    VK_ILOAD(vkCmdSetScissor);
    VK_ILOAD(vkCmdCopyBuffer);
    VK_ILOAD(vkCmdCopyBufferToImage);
    VK_ILOAD(vkCmdCopyImageToBuffer);
    VK_ILOAD(vkCmdPipelineBarrier);
    VK_ILOAD(vkCmdBlitImage);
    VK_ILOAD(vkWaitForFences);
    VK_ILOAD(vkResetFences);
    VK_ILOAD(vkCreateRenderPass);
    VK_ILOAD(vkDestroyRenderPass);
    VK_ILOAD(vkCreateFramebuffer);
    VK_ILOAD(vkDestroyFramebuffer);
    VK_ILOAD(vkCreateDescriptorPool);
    VK_ILOAD(vkDestroyDescriptorPool);
    VK_ILOAD(vkCreateDescriptorSetLayout);
    VK_ILOAD(vkDestroyDescriptorSetLayout);
    VK_ILOAD(vkAllocateDescriptorSets);
    VK_ILOAD(vkFreeDescriptorSets);
    VK_ILOAD(vkUpdateDescriptorSets);
    VK_ILOAD(vkCreateSemaphore);
    VK_ILOAD(vkDestroySemaphore);
    VK_ILOAD(vkCreateFence);
    VK_ILOAD(vkDestroyFence);
    VK_ILOAD(vkCreateDebugUtilsMessengerEXT);
    VK_ILOAD(vkDestroyDebugUtilsMessengerEXT);

    #undef VK_ILOAD

    std::cout << "[VkDispatch] Post-instance Vulkan functions loaded" << std::endl;
#endif
    // Windows: loadFromVulkanDll 已一次加载所有函数，不需要 post-instance
}

} // namespace QymEngine
