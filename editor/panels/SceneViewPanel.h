#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace QymEngine {

class Renderer;

class SceneViewPanel {
public:
    void onImGuiRender(Renderer& renderer);
    void cleanup();

    /// Called before drawScene() each frame to apply pending resize.
    void applyPendingResize(Renderer& renderer);

private:
    void recreateDescriptorSet(Renderer& renderer);

    uint32_t        m_width  = 0;
    uint32_t        m_height = 0;

    uint32_t        m_pendingWidth  = 0;
    uint32_t        m_pendingHeight = 0;
    bool            m_resizePending = false;

    VkDescriptorSet m_descriptorSet   = VK_NULL_HANDLE;
    VkImageView     m_cachedImageView = VK_NULL_HANDLE;
};

} // namespace QymEngine
