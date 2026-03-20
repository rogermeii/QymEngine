#pragma once

#include <vulkan/vulkan.h>
#include <vector>

namespace QymEngine {

class Renderer;

class ImGuiLayer {
public:
    void init(Renderer& renderer);
    void shutdown();

    void beginFrame();
    void endFrame(VkCommandBuffer cmd, uint32_t imageIndex);
    void enableDocking();

    /// Must be called when the swapchain is recreated (e.g. window resize).
    void onSwapChainRecreated(Renderer& renderer);

private:
    void createRenderPass(VkDevice device, VkFormat imageFormat);
    void createFramebuffers(VkDevice device, const std::vector<VkImageView>& imageViews,
                            VkExtent2D extent);

    VkDevice                   m_device     = VK_NULL_HANDLE;
    VkDescriptorPool           m_pool       = VK_NULL_HANDLE;
    VkRenderPass               m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
    VkExtent2D                 m_extent{};
};

} // namespace QymEngine
