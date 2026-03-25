#pragma once
#include <vulkan/vulkan.h>
#include <array>

namespace QymEngine {

class VulkanContext;
class DescriptorLayoutCache;
class CommandManager;

class IBLGenerator {
public:
    void init(VulkanContext& ctx, DescriptorLayoutCache& layoutCache, CommandManager& cmdMgr);
    void destroy();

    // 从全景 HDR 纹理生成所有 IBL 贴图 (cubemap + irradiance + prefiltered)
    void generate(VkImageView panoramaView, VkSampler panoramaSampler);

    // 生成 BRDF LUT（只需调用一次，与场景/天空盒无关）
    void generateBrdfLut();

    // IBL 是否已生成
    bool isGenerated() const { return m_generated; }

    // Getter
    VkImageView getIrradianceView()  const { return m_irradianceCubeView; }
    VkImageView getPrefilteredView() const { return m_prefilteredCubeView; }
    VkImageView getBrdfLutView()     const { return m_brdfLutView; }
    VkSampler   getCubemapSampler()  const { return m_cubemapSampler; }
    VkSampler   getBrdfLutSampler()  const { return m_brdfLutSampler; }

private:
    // 资源维度常量
    static constexpr uint32_t CUBEMAP_SIZE      = 512;
    static constexpr uint32_t IRRADIANCE_SIZE   = 32;
    static constexpr uint32_t PREFILTER_SIZE    = 128;
    static constexpr uint32_t BRDF_LUT_SIZE     = 512;
    static constexpr uint32_t PREFILTER_MIP_LEVELS = 5;

    void createResources();
    void destroyResources();
    void createRenderPasses();
    void destroyRenderPasses();
    void createPipelines();
    void destroyPipelines();

    void generateCubemap(VkImageView panoramaView, VkSampler panoramaSampler);
    void generateIrradiance();
    void generatePrefiltered();

    // 辅助函数
    void createCubemapImage(VkImage& image, VkDeviceMemory& memory, uint32_t size,
                            uint32_t mipLevels, VkFormat format);
    VkImageView createCubeFaceView(VkImage image, VkFormat format, uint32_t face, uint32_t mip);
    VkImageView createCubeView(VkImage image, VkFormat format, uint32_t mipLevels);
    VkFramebuffer createFramebuffer(VkRenderPass rp, VkImageView faceView,
                                     uint32_t width, uint32_t height);
    void transitionImageLayout(VkCommandBuffer cmd, VkImage image,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels, uint32_t layerCount);

    // Vulkan 上下文
    VulkanContext* m_context = nullptr;
    DescriptorLayoutCache* m_layoutCache = nullptr;
    CommandManager* m_cmdMgr = nullptr;

    // --- Cubemap (中间产物) ---
    VkImage          m_cubemapImage      = VK_NULL_HANDLE;
    VkDeviceMemory   m_cubemapMemory     = VK_NULL_HANDLE;
    VkImageView      m_cubemapCubeView   = VK_NULL_HANDLE; // VK_IMAGE_VIEW_TYPE_CUBE
    VkImageView      m_cubemapFaceViews[6] = {};

    // --- Irradiance cubemap ---
    VkImage          m_irradianceImage      = VK_NULL_HANDLE;
    VkDeviceMemory   m_irradianceMemory     = VK_NULL_HANDLE;
    VkImageView      m_irradianceCubeView   = VK_NULL_HANDLE;
    VkImageView      m_irradianceFaceViews[6] = {};

    // --- Pre-filtered cubemap ---
    VkImage          m_prefilteredImage      = VK_NULL_HANDLE;
    VkDeviceMemory   m_prefilteredMemory     = VK_NULL_HANDLE;
    VkImageView      m_prefilteredCubeView   = VK_NULL_HANDLE;
    // 6 面 × 5 mip = 30 个 face view
    VkImageView      m_prefilteredFaceViews[6 * PREFILTER_MIP_LEVELS] = {};

    // --- BRDF LUT ---
    VkImage          m_brdfLutImage      = VK_NULL_HANDLE;
    VkDeviceMemory   m_brdfLutMemory     = VK_NULL_HANDLE;
    VkImageView      m_brdfLutView       = VK_NULL_HANDLE;

    // --- Sampler ---
    VkSampler        m_cubemapSampler    = VK_NULL_HANDLE;
    VkSampler        m_brdfLutSampler    = VK_NULL_HANDLE;

    // --- Render pass ---
    VkRenderPass     m_hdrRenderPass     = VK_NULL_HANDLE; // R16G16B16A16_SFLOAT
    VkRenderPass     m_brdfRenderPass    = VK_NULL_HANDLE; // R16G16_SFLOAT

    // --- Pipeline ---
    VkPipeline       m_equirectPipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_equirectPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_irradiancePipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_irradiancePipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_prefilterPipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_prefilterPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_brdfPipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_brdfPipelineLayout = VK_NULL_HANDLE;

    // --- Descriptor ---
    VkDescriptorSetLayout m_samplerSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descriptorPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_equirectSet      = VK_NULL_HANDLE;
    VkDescriptorSet       m_irradianceSet    = VK_NULL_HANDLE;
    VkDescriptorSet       m_prefilterSet     = VK_NULL_HANDLE;

    bool m_generated = false;
};

} // namespace QymEngine
