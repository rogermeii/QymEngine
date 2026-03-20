#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <imgui.h>
#include <ImGuizmo.h>

namespace QymEngine {

class Renderer;
class Camera;
class Scene;

class SceneViewPanel {
public:
    void onImGuiRender(Renderer& renderer, Camera& camera, Scene& scene);
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

    ImGuizmo::OPERATION m_gizmoOperation = ImGuizmo::TRANSLATE;
};

} // namespace QymEngine
