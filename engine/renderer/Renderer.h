#pragma once

#include "renderer/VulkanContext.h"
#include "renderer/SwapChain.h"
#include "renderer/RenderPass.h"
#include "renderer/Pipeline.h"
#include "renderer/Buffer.h"
#include "renderer/Texture.h"
#include "renderer/Descriptor.h"
#include "renderer/CommandManager.h"
#include "renderer/MeshLibrary.h"
#include "renderer/DescriptorLayoutCache.h"
#include "asset/AssetManager.h"
#include "scene/Scene.h"
#include "scene/Camera.h"

#include <functional>

namespace QymEngine {

class Window;

class Renderer {
public:
    using SwapChainRecreatedCallback = std::function<void()>;

    void init(Window& window);
    bool beginFrame();
    void drawScene(Scene& scene);
    void blitToSwapchain();
    void endFrame();
    void shutdown();

    VulkanContext&  getContext()               { return m_context; }
    SwapChain&      getSwapChain()             { return m_swapChain; }
    CommandManager& getCommandManager()        { return m_commandManager; }
    AssetManager&   getAssetManager()          { return m_assetManager; }
    VkCommandBuffer getCurrentCommandBuffer()  { return m_commandManager.getBuffer(m_currentFrame); }
    uint32_t        getImageIndex()      const { return m_currentImageIndex; }
    uint32_t        getCurrentFrame()    const { return m_currentFrame; }
    Window*         getWindow()                { return m_window; }

    static constexpr int getMaxFramesInFlight() { return MAX_FRAMES_IN_FLIGHT; }

    void setSwapChainRecreatedCallback(SwapChainRecreatedCallback cb) { m_swapChainRecreatedCb = std::move(cb); }

    void setCamera(const Camera* camera) { m_camera = camera; }

    // Shader hot reload: recompile shaders and rebuild pipelines
    void reloadShaders();

    // --- Bindless material system (PC only) ---
    bool isBindlessEnabled() const { return m_bindlessEnabled; }
    uint32_t registerBindlessTexture(VkImageView view);
    uint32_t registerBindlessSampler(VkSampler sampler);
    void updateBindlessMaterialEntry(uint32_t index, const struct BindlessMaterialEntry& entry);
    uint32_t allocateBindlessMaterialIndex();
    VkDescriptorSetLayout getBindlessSetLayout() const { return m_bindlessSetLayout; }
    VkDescriptorSet getBindlessSet() const { return m_bindlessSet; }

    // --- Offscreen rendering ---
    void createOffscreen(uint32_t width, uint32_t height);
    void resizeOffscreen(uint32_t width, uint32_t height);
    void destroyOffscreen();
    void drawSceneToOffscreen(VkCommandBuffer cmd, Scene& scene);

    VkSampler   getOffscreenSampler()   const { return m_offscreenSampler; }
    VkImageView getOffscreenImageView() const { return m_offscreenImageView; }
    VkImage     getOffscreenImage()     const { return m_offscreenImage; }
    uint32_t    getOffscreenWidth()     const { return m_offscreenWidth; }
    uint32_t    getOffscreenHeight()    const { return m_offscreenHeight; }
    bool        isOffscreenReady()      const { return m_offscreenRenderPass != VK_NULL_HANDLE
                                                    && m_offscreenFramebuffer != VK_NULL_HANDLE; }

    // Expose internals for model preview rendering
    VkRenderPass     getOffscreenRenderPass()    const { return m_offscreenRenderPass; }
    Pipeline&        getOffscreenPipeline()             { return m_offscreenPipeline; }
    Descriptor&      getDescriptor()                    { return m_descriptor; }
    Buffer&          getBuffer()                        { return m_buffer; }
    MeshLibrary&     getMeshLibrary()                   { return m_meshLibrary; }
    VkDescriptorSet  getDefaultMaterialSet()    const { return m_defaultMaterialSet; }
    VkImageView      getWhiteFallbackView()     const { return m_whiteFallbackView; }
    VkImageView      getNormalFallbackView()    const { return m_normalFallbackView; }
    VkSampler        getFallbackSampler()       const { return m_fallbackSampler; }
    DescriptorLayoutCache& getLayoutCache()           { return m_layoutCache; }
    VkDescriptorSetLayout  getPerFrameLayout()  const { return m_perFrameLayout; }

    // Draw statistics (updated per frame in drawSceneToOffscreen)
    uint32_t getLastDrawCallCount() const { return m_lastDrawCallCount; }
    uint32_t getLastTriangleCount() const { return m_lastTriangleCount; }

private:
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void updateUniformBuffer(uint32_t currentImage);
    void recreateSwapChain();
    void createFallbackTextures();

    // Sub-modules (order matters for init/shutdown)
    VulkanContext  m_context;
    SwapChain      m_swapChain;
    CommandManager m_commandManager;
    RenderPass     m_renderPass;
    Descriptor     m_descriptor;
    Pipeline       m_pipeline;
    Buffer         m_buffer;
    Texture        m_texture;
    MeshLibrary    m_meshLibrary;
    AssetManager   m_assetManager;
    DescriptorLayoutCache m_layoutCache;

    // Per-frame layout (cache-managed, not owned)
    VkDescriptorSetLayout m_perFrameLayout = VK_NULL_HANDLE;

    const Camera* m_camera = nullptr;
    Scene* m_activeScene = nullptr;
    Window* m_window = nullptr;

    uint32_t m_currentFrame = 0;
    uint32_t m_currentImageIndex = 0;
    bool     m_framebufferResized = false;

    SwapChainRecreatedCallback m_swapChainRecreatedCb;

    // --- Offscreen resources ---
    VkImage          m_offscreenImage       = VK_NULL_HANDLE;
    VkDeviceMemory   m_offscreenMemory      = VK_NULL_HANDLE;
    VkImageView      m_offscreenImageView   = VK_NULL_HANDLE;
    VkSampler        m_offscreenSampler     = VK_NULL_HANDLE;
    VkFramebuffer    m_offscreenFramebuffer = VK_NULL_HANDLE;
    VkRenderPass     m_offscreenRenderPass  = VK_NULL_HANDLE;
    uint32_t         m_offscreenWidth  = 0;
    uint32_t         m_offscreenHeight = 0;

    // Depth buffer for offscreen rendering
    VkImage          m_offscreenDepthImage      = VK_NULL_HANDLE;
    VkDeviceMemory   m_offscreenDepthMemory     = VK_NULL_HANDLE;
    VkImageView      m_offscreenDepthImageView  = VK_NULL_HANDLE;

    // Pipeline for offscreen render pass
    Pipeline         m_offscreenPipeline;
    Pipeline         m_wireframePipeline;

    // Grid pipeline (no vertex input, alpha blending, UBO-only layout)
    VkPipeline       m_gridPipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_gridPipelineLayout = VK_NULL_HANDLE;

    // Fallback textures for materials without maps
    VkImage          m_whiteFallbackImage     = VK_NULL_HANDLE;
    VkDeviceMemory   m_whiteFallbackMemory    = VK_NULL_HANDLE;
    VkImageView      m_whiteFallbackView      = VK_NULL_HANDLE;
    VkSampler        m_fallbackSampler        = VK_NULL_HANDLE;
    VkImage          m_normalFallbackImage    = VK_NULL_HANDLE;
    VkDeviceMemory   m_normalFallbackMemory   = VK_NULL_HANDLE;
    VkImageView      m_normalFallbackView     = VK_NULL_HANDLE;

    // Draw statistics
    uint32_t m_lastDrawCallCount = 0;
    uint32_t m_lastTriangleCount = 0;

    // --- Bindless resources (PC only) ---
    bool m_bindlessEnabled = false;
    VkDescriptorSetLayout m_bindlessSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_bindlessSet = VK_NULL_HANDLE;
    VkDescriptorPool m_bindlessPool = VK_NULL_HANDLE;
    VkBuffer m_materialSSBO = VK_NULL_HANDLE;
    VkDeviceMemory m_materialSSBOMemory = VK_NULL_HANDLE;
    void* m_materialSSBOMapped = nullptr;
    uint32_t m_nextTextureIndex = 0;
    uint32_t m_nextSamplerIndex = 0;
    uint32_t m_nextMaterialIndex = 0;
    static constexpr uint32_t MAX_BINDLESS_TEXTURES = 4096;
    static constexpr uint32_t MAX_BINDLESS_SAMPLERS = 256;
    static constexpr uint32_t MAX_MATERIALS = 256;

    // Bindless pipelines (separate from non-bindless)
    Pipeline m_bindlessOffscreenPipeline;
    Pipeline m_bindlessWireframePipeline;

    // Bindless default material index
    uint32_t m_defaultBindlessMaterialIndex = 0;
    uint32_t m_wireframeBindlessMaterialIndex = 0;

    void createBindlessResources();
    void destroyBindlessResources();

    // Default material descriptor set (for nodes without material)
    VkDescriptorSet  m_defaultMaterialSet     = VK_NULL_HANDLE;
    VkBuffer         m_defaultMaterialParamBuffer = VK_NULL_HANDLE;
    VkDeviceMemory   m_defaultMaterialParamMemory = VK_NULL_HANDLE;
    void*            m_defaultMaterialParamMapped = nullptr;

    // Wireframe highlight material (orange baseColor)
    VkDescriptorSet  m_wireframeMaterialSet   = VK_NULL_HANDLE;
    VkBuffer         m_wireframeMaterialParamBuffer = VK_NULL_HANDLE;
    VkDeviceMemory   m_wireframeMaterialParamMemory = VK_NULL_HANDLE;
    void*            m_wireframeMaterialParamMapped = nullptr;
};

} // namespace QymEngine
