#pragma once
#include <vulkan/vulkan.h>
#include <vector>

struct GLFWwindow;

namespace QymEngine {

class VulkanContext;

struct SwapChainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class SwapChain {
public:
    void create(VulkanContext& ctx, GLFWwindow* window);
    void createFramebuffers(VkDevice device, VkRenderPass renderPass);
    void createSyncObjects(VkDevice device, int maxFramesInFlight);
    void cleanup(VkDevice device);
    void cleanupFramebuffers(VkDevice device);
    void cleanupSyncObjects(VkDevice device, int maxFramesInFlight);

    VkSwapchainKHR          getSwapChain()        const { return m_swapChain; }
    VkFormat                getImageFormat()      const { return m_imageFormat; }
    VkExtent2D              getExtent()           const { return m_extent; }
    const std::vector<VkFramebuffer>& getFramebuffers() const { return m_framebuffers; }

    VkSemaphore getImageAvailableSemaphore(uint32_t frame) const { return m_imageAvailableSemaphores[frame]; }
    VkSemaphore getRenderFinishedSemaphore(uint32_t frame) const { return m_renderFinishedSemaphores[frame]; }
    VkFence     getInFlightFence(uint32_t frame)           const { return m_inFlightFences[frame]; }

    static SwapChainSupportDetails querySupport(VkPhysicalDevice device, VkSurfaceKHR surface);

private:
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR   chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D         chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window);
    VkImageView        createImageView(VkDevice device, VkImage image, VkFormat format);

    VkSwapchainKHR           m_swapChain = VK_NULL_HANDLE;
    std::vector<VkImage>     m_images;
    VkFormat                 m_imageFormat{};
    VkExtent2D               m_extent{};
    std::vector<VkImageView> m_imageViews;
    std::vector<VkFramebuffer> m_framebuffers;

    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence>     m_inFlightFences;
};

} // namespace QymEngine
