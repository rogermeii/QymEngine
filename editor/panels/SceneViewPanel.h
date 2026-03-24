#pragma once

#include <vulkan/vulkan.h>
#include <SDL.h>
#include <cstdint>
#include <functional>
#include <imgui.h>
#include <ImGuizmo.h>

namespace QymEngine {

class Renderer;
class Camera;
class Scene;

class SceneViewPanel {
public:
    using SaveStateFn = std::function<void()>;

    void onImGuiRender(Renderer& renderer, Camera& camera, Scene& scene);
    void cleanup();

    /// Called before drawScene() each frame to apply pending resize.
    void applyPendingResize(Renderer& renderer);

    /// Process SDL event for touch gestures. Call from event callback.
    void processEvent(const SDL_Event& event);

    void setSaveStateFn(SaveStateFn fn) { m_saveState = fn; }

    // Gizmo mode access (for automation)
    ImGuizmo::OPERATION getGizmoOperation() const { return m_gizmoOperation; }
    void setGizmoOperation(ImGuizmo::OPERATION op) { m_gizmoOperation = op; }

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
    bool m_wasGizmoUsing = false;
    SaveStateFn m_saveState;

    // Touch gesture state
    int m_fingerCount = 0;
    float m_pinchDist = 0.0f;
    float m_pinchCenterX = 0.0f;
    float m_pinchCenterY = 0.0f;
    float m_pendingZoom = 0.0f;
    float m_pendingPanX = 0.0f;
    float m_pendingPanY = 0.0f;
    bool m_hasPinchPrev = false;
};

} // namespace QymEngine
