#include "SceneViewPanel.h"
#include "renderer/Renderer.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace QymEngine {

void SceneViewPanel::applyPendingResize(Renderer& renderer)
{
    if (!m_resizePending)
        return;

    m_resizePending = false;

    // Remove old descriptor set before the old image view is destroyed
    if (m_descriptorSet != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkan_RemoveTexture(m_descriptorSet);
        m_descriptorSet   = VK_NULL_HANDLE;
        m_cachedImageView = VK_NULL_HANDLE;
    }

    m_width  = m_pendingWidth;
    m_height = m_pendingHeight;
    renderer.resizeOffscreen(m_width, m_height);

    // Create new descriptor set for the new image view
    recreateDescriptorSet(renderer);
}

void SceneViewPanel::onImGuiRender(Renderer& renderer, Camera& camera, Scene& scene)
{
    ImGui::Begin("Scene View");

    ImVec2 viewportSize = ImGui::GetContentRegionAvail();

    // Minimum size to prevent zero-size framebuffer
    if (viewportSize.x < 1.0f || viewportSize.y < 1.0f)
    {
        ImGui::End();
        return;
    }

    uint32_t w = static_cast<uint32_t>(viewportSize.x);
    uint32_t h = static_cast<uint32_t>(viewportSize.y);

    // Schedule resize for next frame if size changed
    if (w != m_width || h != m_height)
    {
        m_pendingWidth  = w;
        m_pendingHeight = h;
        m_resizePending = true;
    }

    // Ensure we have a valid descriptor set
    if (m_descriptorSet == VK_NULL_HANDLE && renderer.isOffscreenReady())
    {
        recreateDescriptorSet(renderer);
    }

    // Display offscreen render result
    if (m_descriptorSet != VK_NULL_HANDLE)
    {
        ImVec2 displaySize(static_cast<float>(m_width), static_cast<float>(m_height));
        ImGui::Image(reinterpret_cast<ImTextureID>(m_descriptorSet), displaySize);
    }

    // --- Gizmo ---
    Node* selected = scene.getSelectedNode();
    if (selected && selected->meshType != MeshType::None) {
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();

        // Get the Scene View panel's screen position and size
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 windowSize = ImGui::GetWindowSize();

        // Account for title bar offset
        ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
        float titleBarHeight = contentMin.y;

        ImGuizmo::SetRect(windowPos.x, windowPos.y + titleBarHeight,
                          viewportSize.x, viewportSize.y);

        // Get camera matrices
        glm::mat4 view = camera.getViewMatrix();
        float aspect = viewportSize.x / viewportSize.y;
        glm::mat4 proj = camera.getProjMatrix(aspect);

        // Get node's world transform
        glm::mat4 worldMatrix = selected->getWorldMatrix();

        // Draw and manipulate gizmo
        ImGuizmo::Manipulate(
            glm::value_ptr(view),
            glm::value_ptr(proj),
            m_gizmoOperation,
            ImGuizmo::LOCAL,
            glm::value_ptr(worldMatrix)
        );

        // If gizmo was used, decompose back to local transform
        if (ImGuizmo::IsUsing()) {
            // Convert world matrix back to local
            glm::mat4 localMatrix = worldMatrix;
            if (selected->getParent()) {
                glm::mat4 parentWorld = selected->getParent()->getWorldMatrix();
                localMatrix = glm::inverse(parentWorld) * worldMatrix;
            }

            // Decompose local matrix into position/rotation/scale
            glm::vec3 translation, scale, skew;
            glm::quat rotation;
            glm::vec4 perspective;
            glm::decompose(localMatrix, scale, rotation, translation, skew, perspective);

            selected->transform.position = translation;
            selected->transform.scale = scale;

            // Convert quaternion to euler angles (degrees)
            glm::vec3 eulerRad = glm::eulerAngles(rotation);
            selected->transform.rotation = glm::degrees(eulerRad);
        }
    }

    // Keyboard shortcuts for gizmo mode (when Scene View is focused)
    if (ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_W))
            m_gizmoOperation = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E))
            m_gizmoOperation = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R))
            m_gizmoOperation = ImGuizmo::SCALE;
    }

    // Camera input (only when hovering AND gizmo is not active)
    if (ImGui::IsWindowHovered() && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver()) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            camera.orbit(io.MouseDelta.x * 0.3f, io.MouseDelta.y * 0.3f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            camera.pan(-io.MouseDelta.x * 0.003f, io.MouseDelta.y * 0.003f);
        }
        if (io.MouseWheel != 0.0f) {
            camera.zoom(-io.MouseWheel * 0.5f);
        }
    }

    ImGui::End();
}

void SceneViewPanel::cleanup()
{
    if (m_descriptorSet != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkan_RemoveTexture(m_descriptorSet);
        m_descriptorSet   = VK_NULL_HANDLE;
        m_cachedImageView = VK_NULL_HANDLE;
    }
}

void SceneViewPanel::recreateDescriptorSet(Renderer& renderer)
{
    VkImageView currentView = renderer.getOffscreenImageView();
    VkSampler   sampler     = renderer.getOffscreenSampler();

    if (currentView == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE)
        return;

    if (currentView == m_cachedImageView && m_descriptorSet != VK_NULL_HANDLE)
        return;

    if (m_descriptorSet != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkan_RemoveTexture(m_descriptorSet);
        m_descriptorSet = VK_NULL_HANDLE;
    }

    m_descriptorSet = ImGui_ImplVulkan_AddTexture(
        sampler, currentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_cachedImageView = currentView;
}

} // namespace QymEngine
