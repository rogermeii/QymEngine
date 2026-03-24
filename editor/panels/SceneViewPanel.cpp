#include "SceneViewPanel.h"
#include "renderer/VkDispatch.h"
#include "UIAutomation.h"
#include "renderer/Renderer.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>

#include <SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>

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
#ifndef __ANDROID__
    UIAutomation::recordPanel("SceneView");
#endif

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
        // OpenGL FBO 纹理 Y 轴是从下到上，ImGui UV (0,0) 在左上
        // 需要翻转 UV Y: uv0=(0,1) uv1=(1,0)
        if (QymEngine::vkIsOpenGLBackend() || QymEngine::vkIsGLESBackend())
            ImGui::Image(reinterpret_cast<ImTextureID>(m_descriptorSet), displaySize, ImVec2(0,1), ImVec2(1,0));
        else
            ImGui::Image(reinterpret_cast<ImTextureID>(m_descriptorSet), displaySize);
    }

    // --- Orientation axis gizmo (top-right corner) ---
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 contentMin = ImGui::GetWindowContentRegionMin();

        float gizmoSize = 60.0f;
        float padding = 15.0f;
        ImVec2 center = ImVec2(
            winPos.x + contentMin.x + viewportSize.x - gizmoSize - padding + gizmoSize * 0.5f,
            winPos.y + contentMin.y + padding + gizmoSize * 0.5f
        );

        // Get camera rotation to transform axes
        glm::mat4 viewMat = camera.getViewMatrix();
        glm::mat3 rot = glm::mat3(viewMat); // extract rotation part

        float axisLen = gizmoSize * 0.4f;

        // X axis (red)
        glm::vec3 xEnd = rot * glm::vec3(1, 0, 0);
        ImVec2 xScreen = ImVec2(center.x + xEnd.x * axisLen, center.y - xEnd.y * axisLen);
        drawList->AddLine(center, xScreen, IM_COL32(255, 60, 60, 255), 2.0f);
        drawList->AddText(ImVec2(xScreen.x - 4, xScreen.y - 8), IM_COL32(255, 60, 60, 255), "X");

        // Y axis (green)
        glm::vec3 yEnd = rot * glm::vec3(0, 1, 0);
        ImVec2 yScreen = ImVec2(center.x + yEnd.x * axisLen, center.y - yEnd.y * axisLen);
        drawList->AddLine(center, yScreen, IM_COL32(60, 255, 60, 255), 2.0f);
        drawList->AddText(ImVec2(yScreen.x - 4, yScreen.y - 8), IM_COL32(60, 255, 60, 255), "Y");

        // Z axis (blue)
        glm::vec3 zEnd = rot * glm::vec3(0, 0, 1);
        ImVec2 zScreen = ImVec2(center.x + zEnd.x * axisLen, center.y - zEnd.y * axisLen);
        drawList->AddLine(center, zScreen, IM_COL32(60, 60, 255, 255), 2.0f);
        drawList->AddText(ImVec2(zScreen.x - 4, zScreen.y - 8), IM_COL32(60, 60, 255, 255), "Z");
    }

    // --- Gizmo ---
    Node* selected = scene.getSelectedNode();
    if (selected) {
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::AllowAxisFlip(false);
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
        // Undo Vulkan Y-flip for ImGuizmo (it uses OpenGL conventions internally)
        proj[1][1] *= -1;

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

        // Save undo state when gizmo starts being used
        bool gizmoUsing = ImGuizmo::IsUsing();
        if (gizmoUsing && !m_wasGizmoUsing && m_saveState)
            m_saveState();
        m_wasGizmoUsing = gizmoUsing;

        // If gizmo was used, decompose back to local transform
        if (gizmoUsing) {
            // Convert world matrix back to local
            glm::mat4 localMatrix = worldMatrix;
            if (selected->getParent()) {
                glm::mat4 parentWorld = selected->getParent()->getWorldMatrix();
                localMatrix = glm::inverse(parentWorld) * worldMatrix;
            }

            // Extract position from column 3
            selected->transform.position = glm::vec3(localMatrix[3]);

            // Extract scale from column lengths
            glm::vec3 colX(localMatrix[0]);
            glm::vec3 colY(localMatrix[1]);
            glm::vec3 colZ(localMatrix[2]);
            glm::vec3 scale(glm::length(colX), glm::length(colY), glm::length(colZ));

            // Handle reflection (negative determinant)
            glm::mat3 rot3(localMatrix);
            if (glm::determinant(rot3) < 0.0f)
                scale.x = -scale.x;

            selected->transform.scale = scale;

            // Normalize columns to get pure rotation matrix
            glm::mat3 r;
            r[0] = glm::vec3(localMatrix[0]) / scale.x;
            r[1] = glm::vec3(localMatrix[1]) / scale.y;
            r[2] = glm::vec3(localMatrix[2]) / scale.z;

            // Extract XYZ intrinsic euler angles matching getLocalMatrix() order:
            // R = Rx(a) * Ry(b) * Rz(c)  =>  r[2][0] = sin(b)
            float sinY = glm::clamp(r[2][0], -1.0f, 1.0f);
            float y = std::asin(sinY);
            float cosY = std::cos(y);
            float x, z;
            if (std::abs(cosY) > 0.0001f) {
                x = std::atan2(-r[2][1], r[2][2]);
                z = std::atan2(-r[1][0], r[0][0]);
            } else {
                x = std::atan2(r[1][2], r[1][1]);
                z = 0.0f;
            }
            selected->transform.rotation = glm::degrees(glm::vec3(x, y, z));
        }
    }

    // Camera & keyboard input (when hovering Scene View, gizmo not active)
    if (ImGui::IsWindowHovered() && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver()) {
        ImGuiIO& io = ImGui::GetIO();

        // --- Consume accumulated touch gesture deltas (from processEvent) ---
        if (std::abs(m_pendingZoom) > 0.001f) {
            camera.zoom(m_pendingZoom);
        }
        if (std::abs(m_pendingPanX) > 0.001f || std::abs(m_pendingPanY) > 0.001f) {
            camera.pan(m_pendingPanX, m_pendingPanY);
        }

        // Single finger / mouse input (only when no multi-touch active)
        if (m_fingerCount < 2) {
            bool rightMouseHeld = ImGui::IsMouseDown(ImGuiMouseButton_Right);

            // Right-click + WASD: fly camera (Shift to boost)
            if (rightMouseHeld) {
                float speed = io.KeyShift ? 8.0f : 2.5f;
                speed *= io.DeltaTime;
                glm::vec3 forward = glm::normalize(camera.target - camera.getPosition());
                glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));

                glm::vec3 move(0.0f);
                if (ImGui::IsKeyDown(ImGuiKey_W)) move += forward * speed;
                if (ImGui::IsKeyDown(ImGuiKey_S)) move -= forward * speed;
                if (ImGui::IsKeyDown(ImGuiKey_A)) move -= right * speed;
                if (ImGui::IsKeyDown(ImGuiKey_D)) move += right * speed;

                camera.target += move;

                if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                    camera.orbit(io.MouseDelta.x * 0.3f, io.MouseDelta.y * 0.3f);
                }
            } else {
                // Single finger drag (touch as left mouse): orbit
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    camera.orbit(io.MouseDelta.x * 0.3f, io.MouseDelta.y * 0.3f);
                }

                // W/E/R: gizmo mode shortcuts
                if (ImGui::IsKeyPressed(ImGuiKey_W))
                    m_gizmoOperation = ImGuizmo::TRANSLATE;
                if (ImGui::IsKeyPressed(ImGuiKey_E))
                    m_gizmoOperation = ImGuizmo::ROTATE;
                if (ImGui::IsKeyPressed(ImGuiKey_R))
                    m_gizmoOperation = ImGuizmo::SCALE;
            }

            // Middle-click drag: pan (desktop)
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                camera.pan(-io.MouseDelta.x * 0.003f, io.MouseDelta.y * 0.003f);
            }
            // Scroll: zoom (desktop)
            if (io.MouseWheel != 0.0f) {
                camera.zoom(-io.MouseWheel * 0.5f);
            }
        }
    }

    // Reset per-frame gesture accumulators
    m_pendingZoom = 0.0f;
    m_pendingPanX = 0.0f;
    m_pendingPanY = 0.0f;

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

void SceneViewPanel::processEvent(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_FINGERDOWN:
        m_fingerCount++;
        m_hasPinchPrev = false; // Reset pinch tracking when finger count changes
        break;

    case SDL_FINGERUP:
        m_fingerCount--;
        if (m_fingerCount < 0) m_fingerCount = 0;
        m_hasPinchPrev = false;
        break;

    case SDL_FINGERMOTION:
        if (m_fingerCount >= 2) {
            // Get all current fingers for pinch/pan calculation
            SDL_TouchID touchId = event.tfinger.touchId;
            int numFingers = SDL_GetNumTouchFingers(touchId);
            if (numFingers >= 2) {
                SDL_Finger* f0 = SDL_GetTouchFinger(touchId, 0);
                SDL_Finger* f1 = SDL_GetTouchFinger(touchId, 1);
                if (f0 && f1) {
                    float dx = f1->x - f0->x;
                    float dy = f1->y - f0->y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    float cx = (f0->x + f1->x) * 0.5f;
                    float cy = (f0->y + f1->y) * 0.5f;

                    if (m_hasPinchPrev) {
                        // Pinch zoom (distance change)
                        float deltaDist = dist - m_pinchDist;
                        m_pendingZoom += -deltaDist * 5.0f;

                        // Two-finger pan (center movement)
                        float panDx = cx - m_pinchCenterX;
                        float panDy = cy - m_pinchCenterY;
                        m_pendingPanX += -panDx * 2.0f;
                        m_pendingPanY += panDy * 2.0f;
                    }

                    m_pinchDist = dist;
                    m_pinchCenterX = cx;
                    m_pinchCenterY = cy;
                    m_hasPinchPrev = true;
                }
            }
        }
        break;
    }
}

} // namespace QymEngine
