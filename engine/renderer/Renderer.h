#pragma once

#include "renderer/VulkanContext.h"
#include "renderer/SwapChain.h"
#include "renderer/RenderPass.h"
#include "renderer/Pipeline.h"
#include "renderer/Buffer.h"
#include "renderer/Texture.h"
#include "renderer/Descriptor.h"
#include "renderer/CommandManager.h"

namespace QymEngine {

class Window;

class Renderer {
public:
    void init(Window& window);
    bool beginFrame();
    void drawScene();
    void endFrame();
    void shutdown();

private:
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void updateUniformBuffer(uint32_t currentImage);
    void recreateSwapChain();

    // Sub-modules (order matters for init/shutdown)
    VulkanContext  m_context;
    SwapChain      m_swapChain;
    CommandManager m_commandManager;
    RenderPass     m_renderPass;
    Descriptor     m_descriptor;
    Pipeline       m_pipeline;
    Buffer         m_buffer;
    Texture        m_texture;

    Window* m_window = nullptr;

    uint32_t m_currentFrame = 0;
    uint32_t m_currentImageIndex = 0;
    bool     m_framebufferResized = false;
};

} // namespace QymEngine
