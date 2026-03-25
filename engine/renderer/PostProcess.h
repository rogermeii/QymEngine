#pragma once
#include <algorithm>
#include <vulkan/vulkan.h>

namespace QymEngine {

static constexpr int MAX_BLOOM_MIPS = 6;

struct PostProcessSettings {
    // Bloom
    bool  bloomEnabled = true;
    float bloomThreshold = 0.9f;
    float bloomIntensity = 1.0f;
    int   bloomMipCount = 5;

    // Tone Mapping
    bool  toneMappingEnabled = true;
    float exposure = 1.0f;

    // Color Grading
    bool  colorGradingEnabled = true;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float temperature = 0.0f;
    float tint = 0.0f;
    float brightness = 0.0f;

    // FXAA
    bool  fxaaEnabled = true;
    float fxaaSubpixQuality = 0.75f;
    float fxaaEdgeThreshold = 0.166f;
    float fxaaEdgeThresholdMin = 0.0833f;

    void clampValues() {
        bloomMipCount = std::clamp(bloomMipCount, 1, MAX_BLOOM_MIPS);
        exposure = std::max(exposure, 0.001f);
        contrast = std::clamp(contrast, 0.5f, 2.0f);
        saturation = std::clamp(saturation, 0.0f, 2.0f);
        temperature = std::clamp(temperature, -1.0f, 1.0f);
        tint = std::clamp(tint, -1.0f, 1.0f);
        brightness = std::clamp(brightness, -1.0f, 1.0f);
    }
};

class VulkanContext;
class DescriptorLayoutCache;

class PostProcessPipeline {
public:
    void init(VulkanContext& ctx, DescriptorLayoutCache& layoutCache,
              uint32_t width, uint32_t height);
    void destroy();
    void resize(uint32_t width, uint32_t height);
    void reloadShaders();

    // sceneHDR image 必须处于 SHADER_READ_ONLY_OPTIMAL 布局
    void execute(VkCommandBuffer cmd, VkImageView sceneHDR,
                 const PostProcessSettings& settings);

    VkImage     getFinalImage(const PostProcessSettings& settings) const;
    VkImageView getFinalImageView(const PostProcessSettings& settings) const;

private:
    void executeBloom(VkCommandBuffer cmd, VkImageView sceneHDR,
                      const PostProcessSettings& settings);
    void executeComposite(VkCommandBuffer cmd, VkImageView sceneHDR,
                          VkImageView bloomTexture,
                          const PostProcessSettings& settings);
    void executeFxaa(VkCommandBuffer cmd, const PostProcessSettings& settings);

    void createBloomResources();
    void destroyBloomResources();
    void createLdrResources();
    void destroyLdrResources();
    void createPipelines();
    void destroyPipelines();
    void createBlackFallback();
    void destroyBlackFallback();

    // Bloom 资源 — 降采样链
    VkImage        m_bloomDownMipImage = VK_NULL_HANDLE;
    VkDeviceMemory m_bloomDownMipMemory = VK_NULL_HANDLE;
    VkImageView    m_bloomDownMipViews[MAX_BLOOM_MIPS]{};
    VkFramebuffer  m_bloomDownsampleFBs[MAX_BLOOM_MIPS]{};

    // Bloom 资源 — 升采样链（独立 image，消除跨帧读写冲突）
    VkImage        m_bloomUpMipImage = VK_NULL_HANDLE;
    VkDeviceMemory m_bloomUpMipMemory = VK_NULL_HANDLE;
    VkImageView    m_bloomUpMipViews[MAX_BLOOM_MIPS]{};
    VkFramebuffer  m_bloomUpsampleFBs[MAX_BLOOM_MIPS]{};

    // Composite 资源
    VkImage        m_compositeImage = VK_NULL_HANDLE;
    VkDeviceMemory m_compositeMemory = VK_NULL_HANDLE;
    VkImageView    m_compositeImageView = VK_NULL_HANDLE;
    VkFramebuffer  m_compositeFramebuffer = VK_NULL_HANDLE;

    // FXAA 资源
    VkImage        m_fxaaImage = VK_NULL_HANDLE;
    VkDeviceMemory m_fxaaMemory = VK_NULL_HANDLE;
    VkImageView    m_fxaaImageView = VK_NULL_HANDLE;
    VkFramebuffer  m_fxaaFramebuffer = VK_NULL_HANDLE;

    // 1x1 黑色纹理备用 (bloom 禁用时使用)
    VkImage        m_blackFallbackImage = VK_NULL_HANDLE;
    VkDeviceMemory m_blackFallbackMemory = VK_NULL_HANDLE;
    VkImageView    m_blackFallbackView = VK_NULL_HANDLE;

    // 管线
    VkPipeline       m_bloomDownsamplePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomDownsampleLayout = VK_NULL_HANDLE;
    VkPipeline       m_bloomUpsamplePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomUpsampleLayout = VK_NULL_HANDLE;
    VkPipeline       m_compositePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_compositeLayout = VK_NULL_HANDLE;
    VkPipeline       m_fxaaPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_fxaaLayout = VK_NULL_HANDLE;

    // RenderPass
    VkRenderPass   m_bloomDownsampleRenderPass = VK_NULL_HANDLE;
    VkRenderPass   m_bloomUpsampleRenderPass = VK_NULL_HANDLE;
    VkRenderPass   m_ldrRenderPass = VK_NULL_HANDLE;

    // 共享资源
    VkSampler      m_linearSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_postProcessSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descriptorPool = VK_NULL_HANDLE;

    // 描述符集
    VkDescriptorSet m_bloomDownsampleSets[MAX_BLOOM_MIPS]{};
    VkDescriptorSet m_bloomUpsampleSets[MAX_BLOOM_MIPS]{};
    VkDescriptorSet m_compositeSet = VK_NULL_HANDLE;
    VkDescriptorSet m_fxaaSet = VK_NULL_HANDLE;

    VulkanContext* m_context = nullptr;
    DescriptorLayoutCache* m_layoutCache = nullptr;
    uint32_t m_width = 0, m_height = 0;
    VkImageView m_boundSceneHDR = VK_NULL_HANDLE;  // 缓存已绑定的 sceneHDR view
    int m_lastBloomMipCount = -1;  // 缓存上次使用的 bloom mip 级数
};

} // namespace QymEngine
