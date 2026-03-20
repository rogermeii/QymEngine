#include "SceneViewPanel.h"
#include "renderer/Renderer.h"
#include "scene/Camera.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

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

void SceneViewPanel::onImGuiRender(Renderer& renderer, Camera& camera)
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

    // Camera input (only when hovering Scene View)
    if (ImGui::IsWindowHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            camera.orbit(io.MouseDelta.x * 0.3f, -io.MouseDelta.y * 0.3f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            camera.pan(-io.MouseDelta.x * 0.01f, io.MouseDelta.y * 0.01f);
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
