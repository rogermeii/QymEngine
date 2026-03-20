#pragma once
#include <vulkan/vulkan.h>

namespace QymEngine {

class RenderPass {
public:
    void create(VkDevice device, VkFormat swapChainImageFormat);
    void cleanup(VkDevice device);

    VkRenderPass get() const { return m_renderPass; }

private:
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
};

} // namespace QymEngine
