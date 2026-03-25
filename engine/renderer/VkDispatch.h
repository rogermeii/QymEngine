#pragma once

// ============================================================================
// Vulkan 函数分发层
//
// 所有 vk* 函数通过全局函数指针调用，启动时根据后端选择加载：
//   - Vulkan 模式: 从 vulkan-1.dll 动态加载
//   - D3D12 模式:  使用 D3D12 封装实现
//
// 使用方法: 在所有调用 vk* 函数的 .cpp 文件中 #include "renderer/VkDispatch.h"
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>

namespace QymEngine {

/// 初始化函数分发表
/// @param backendType 0=Vulkan, 1=D3D12, 2=D3D11, 3=OpenGL, 4=GLES
void vkInitDispatch(int backendType);

/// 清理分发表，释放动态库
void vkShutdownDispatch();

/// Vulkan 后端: 创建 instance 后加载实例级函数 (Android/Linux 需要)
void vkPostInstanceInit(VkInstance instance);

/// 查询当前是否使用 D3D12 后端
bool vkIsD3D12Backend();

/// 查询当前是否使用 D3D11 后端
bool vkIsD3D11Backend();

/// 查询当前是否使用 DirectX 后端 (D3D12 或 D3D11)
bool vkIsDirectXBackend();

/// 查询当前是否使用 OpenGL 后端
bool vkIsOpenGLBackend();

/// 查询当前是否使用 GLES 后端
bool vkIsGLESBackend();

/// 查询当前是否使用 Metal 后端
bool vkIsMetalBackend();
bool vkHasClipControl();
void vkSetClipControlSupport(bool enabled);

} // namespace QymEngine

// ============================================================================
// 全局函数指针声明 (91 个)
// ============================================================================

// --- Instance ---
extern PFN_vkCreateInstance                         vkCreateInstance;
extern PFN_vkDestroyInstance                        vkDestroyInstance;
extern PFN_vkEnumerateInstanceExtensionProperties   vkEnumerateInstanceExtensionProperties;
extern PFN_vkEnumerateInstanceLayerProperties       vkEnumerateInstanceLayerProperties;
extern PFN_vkGetInstanceProcAddr                    vkGetInstanceProcAddr;

// --- Physical Device ---
extern PFN_vkEnumeratePhysicalDevices               vkEnumeratePhysicalDevices;
extern PFN_vkGetPhysicalDeviceProperties            vkGetPhysicalDeviceProperties;
extern PFN_vkGetPhysicalDeviceFeatures              vkGetPhysicalDeviceFeatures;
extern PFN_vkGetPhysicalDeviceFeatures2             vkGetPhysicalDeviceFeatures2;
extern PFN_vkGetPhysicalDeviceMemoryProperties      vkGetPhysicalDeviceMemoryProperties;
extern PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
extern PFN_vkEnumerateDeviceExtensionProperties     vkEnumerateDeviceExtensionProperties;

// --- Device & Queue ---
extern PFN_vkCreateDevice                           vkCreateDevice;
extern PFN_vkDestroyDevice                          vkDestroyDevice;
extern PFN_vkGetDeviceProcAddr                      vkGetDeviceProcAddr;
extern PFN_vkGetDeviceQueue                         vkGetDeviceQueue;
extern PFN_vkDeviceWaitIdle                         vkDeviceWaitIdle;

// --- Surface ---
extern PFN_vkDestroySurfaceKHR                      vkDestroySurfaceKHR;
extern PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
extern PFN_vkGetPhysicalDeviceSurfaceFormatsKHR     vkGetPhysicalDeviceSurfaceFormatsKHR;
extern PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;
extern PFN_vkGetPhysicalDeviceSurfaceSupportKHR     vkGetPhysicalDeviceSurfaceSupportKHR;
#ifdef VK_USE_PLATFORM_WIN32_KHR
extern PFN_vkCreateWin32SurfaceKHR                  vkCreateWin32SurfaceKHR;
#endif

// --- Swapchain ---
extern PFN_vkCreateSwapchainKHR                     vkCreateSwapchainKHR;
extern PFN_vkDestroySwapchainKHR                    vkDestroySwapchainKHR;
extern PFN_vkGetSwapchainImagesKHR                  vkGetSwapchainImagesKHR;
extern PFN_vkAcquireNextImageKHR                    vkAcquireNextImageKHR;
extern PFN_vkQueuePresentKHR                        vkQueuePresentKHR;

// --- Buffer ---
extern PFN_vkCreateBuffer                           vkCreateBuffer;
extern PFN_vkDestroyBuffer                          vkDestroyBuffer;
extern PFN_vkGetBufferMemoryRequirements            vkGetBufferMemoryRequirements;

// --- Memory ---
extern PFN_vkAllocateMemory                         vkAllocateMemory;
extern PFN_vkFreeMemory                             vkFreeMemory;
extern PFN_vkBindBufferMemory                       vkBindBufferMemory;
extern PFN_vkMapMemory                              vkMapMemory;
extern PFN_vkUnmapMemory                            vkUnmapMemory;
extern PFN_vkFlushMappedMemoryRanges                vkFlushMappedMemoryRanges;

// --- Image ---
extern PFN_vkCreateImage                            vkCreateImage;
extern PFN_vkDestroyImage                           vkDestroyImage;
extern PFN_vkGetImageMemoryRequirements             vkGetImageMemoryRequirements;
extern PFN_vkBindImageMemory                        vkBindImageMemory;
extern PFN_vkCreateImageView                        vkCreateImageView;
extern PFN_vkDestroyImageView                       vkDestroyImageView;

// --- Sampler ---
extern PFN_vkCreateSampler                          vkCreateSampler;
extern PFN_vkDestroySampler                         vkDestroySampler;

// --- Command Pool & Buffer ---
extern PFN_vkCreateCommandPool                      vkCreateCommandPool;
extern PFN_vkDestroyCommandPool                     vkDestroyCommandPool;
extern PFN_vkResetCommandPool                       vkResetCommandPool;
extern PFN_vkAllocateCommandBuffers                 vkAllocateCommandBuffers;
extern PFN_vkFreeCommandBuffers                     vkFreeCommandBuffers;
extern PFN_vkBeginCommandBuffer                     vkBeginCommandBuffer;
extern PFN_vkEndCommandBuffer                       vkEndCommandBuffer;
extern PFN_vkResetCommandBuffer                     vkResetCommandBuffer;

// --- Command Recording ---
extern PFN_vkCmdBeginRenderPass                     vkCmdBeginRenderPass;
extern PFN_vkCmdEndRenderPass                       vkCmdEndRenderPass;
extern PFN_vkCmdBindPipeline                        vkCmdBindPipeline;
extern PFN_vkCmdBindDescriptorSets                  vkCmdBindDescriptorSets;
extern PFN_vkCmdBindVertexBuffers                   vkCmdBindVertexBuffers;
extern PFN_vkCmdBindIndexBuffer                     vkCmdBindIndexBuffer;
extern PFN_vkCmdDraw                                vkCmdDraw;
extern PFN_vkCmdDrawIndexed                         vkCmdDrawIndexed;
extern PFN_vkCmdSetViewport                         vkCmdSetViewport;
extern PFN_vkCmdSetScissor                          vkCmdSetScissor;
extern PFN_vkCmdPushConstants                       vkCmdPushConstants;
extern PFN_vkCmdCopyBuffer                          vkCmdCopyBuffer;
extern PFN_vkCmdCopyBufferToImage                   vkCmdCopyBufferToImage;
extern PFN_vkCmdCopyImageToBuffer                   vkCmdCopyImageToBuffer;
extern PFN_vkCmdPipelineBarrier                     vkCmdPipelineBarrier;
extern PFN_vkCmdBlitImage                           vkCmdBlitImage;

// --- Synchronization ---
extern PFN_vkWaitForFences                          vkWaitForFences;
extern PFN_vkResetFences                            vkResetFences;
extern PFN_vkQueueSubmit                            vkQueueSubmit;
extern PFN_vkQueueWaitIdle                          vkQueueWaitIdle;

// --- Pipeline ---
extern PFN_vkCreatePipelineLayout                   vkCreatePipelineLayout;
extern PFN_vkDestroyPipelineLayout                  vkDestroyPipelineLayout;
extern PFN_vkCreateShaderModule                     vkCreateShaderModule;
extern PFN_vkDestroyShaderModule                    vkDestroyShaderModule;
extern PFN_vkCreateGraphicsPipelines                vkCreateGraphicsPipelines;
extern PFN_vkDestroyPipeline                        vkDestroyPipeline;

// --- Render Pass & Framebuffer ---
extern PFN_vkCreateRenderPass                       vkCreateRenderPass;
extern PFN_vkDestroyRenderPass                      vkDestroyRenderPass;
extern PFN_vkCreateFramebuffer                      vkCreateFramebuffer;
extern PFN_vkDestroyFramebuffer                     vkDestroyFramebuffer;

// --- Descriptor ---
extern PFN_vkCreateDescriptorPool                   vkCreateDescriptorPool;
extern PFN_vkDestroyDescriptorPool                  vkDestroyDescriptorPool;
extern PFN_vkCreateDescriptorSetLayout              vkCreateDescriptorSetLayout;
extern PFN_vkDestroyDescriptorSetLayout             vkDestroyDescriptorSetLayout;
extern PFN_vkAllocateDescriptorSets                 vkAllocateDescriptorSets;
extern PFN_vkFreeDescriptorSets                     vkFreeDescriptorSets;
extern PFN_vkUpdateDescriptorSets                   vkUpdateDescriptorSets;

// --- Sync Objects ---
extern PFN_vkCreateSemaphore                        vkCreateSemaphore;
extern PFN_vkDestroySemaphore                       vkDestroySemaphore;
extern PFN_vkCreateFence                            vkCreateFence;
extern PFN_vkDestroyFence                           vkDestroyFence;

// --- Debug ---
extern PFN_vkCreateDebugUtilsMessengerEXT           vkCreateDebugUtilsMessengerEXT;
extern PFN_vkDestroyDebugUtilsMessengerEXT          vkDestroyDebugUtilsMessengerEXT;
