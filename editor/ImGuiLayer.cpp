#include "ImGuiLayer.h"
#include "renderer/VkDispatch.h"

#include "renderer/Renderer.h"
#include "renderer/VulkanContext.h"
#include "renderer/SwapChain.h"
#include "core/Window.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>

#include <stdexcept>

namespace QymEngine {

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void ImGuiLayer::init(Renderer& renderer)
{
    VulkanContext& ctx = renderer.getContext();
    SwapChain&     sc  = renderer.getSwapChain();
    m_device  = ctx.getDevice();
    m_extent  = sc.getExtent();

    // --- 1. Descriptor pool for ImGui ---------------------------------------------------
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 100;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    poolInfo.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS)
        throw std::runtime_error("failed to create ImGui descriptor pool!");

    // --- 2. Render pass (clears swapchain; scene renders to offscreen) -------------------
    createRenderPass(m_device, sc.getImageFormat());

    // --- 3. Framebuffers ----------------------------------------------------------------
    createFramebuffers(m_device, sc.getImageViews(), sc.getExtent());

    // --- 4. ImGui context ---------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  // optional multi-viewport

    // Load CJK font
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
#ifdef __ANDROID__
    const char* fontPath = "/system/fonts/NotoSansCJK-Regular.ttc";
    float fontSize = 36.0f;
#else
    const char* fontPath = "C:/Windows/Fonts/msyh.ttc";
    float fontSize = 16.0f;
#endif
    ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath, fontSize, &fontConfig, io.Fonts->GetGlyphRangesChineseFull());
    if (!font) {
        io.FontGlobalScale = 1.5f;
    }

    ImGui::StyleColorsDark();

    // --- 5. Platform / Renderer backends ------------------------------------------------
    ImGui_ImplSDL2_InitForVulkan(renderer.getWindow()->getNativeWindow());

    // 注册 SDL 事件回调，转发给 ImGui
    renderer.getWindow()->setEventCallback([](const SDL_Event& event) {
        ImGui_ImplSDL2_ProcessEvent(&event);
    });

    // VK_NO_PROTOTYPES 模式下，ImGui 需要手动加载 Vulkan 函数指针
    {
        VkInstance vkInstance = ctx.getInstance();
#ifdef __ANDROID__
        // Android: 优先用 SDL 的 vkGetInstanceProcAddr (Vulkan 后端)
        // GLES 后端 fallback 到 VkDispatch 的 vkGetInstanceProcAddr (shim)
        {
            auto sdlGetProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
            auto getProc = sdlGetProcAddr ? sdlGetProcAddr : vkGetInstanceProcAddr;
            if (getProc) {
                ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3,
                    [](const char* name, void* ud) -> PFN_vkVoidFunction {
                        auto* ctx = static_cast<void**>(ud);
                        auto fn = reinterpret_cast<PFN_vkGetInstanceProcAddr>(ctx[0]);
                        auto inst = reinterpret_cast<VkInstance>(ctx[1]);
                        return fn(inst, name);
                    }, (void*)(new void*[2]{(void*)getProc, (void*)vkInstance}));
            }
        }
#else
        // Windows: 通过 VkDispatch 的 vkGetInstanceProcAddr 加载
        ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3,
            [](const char* name, void* ud) -> PFN_vkVoidFunction {
                return vkGetInstanceProcAddr(static_cast<VkInstance>(ud), name);
            }, vkInstance);
#endif
    }

    QueueFamilyIndices indices = ctx.findQueueFamilies(ctx.getPhysicalDevice());

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion      = VK_API_VERSION_1_3;
    initInfo.Instance        = ctx.getInstance();
    initInfo.PhysicalDevice  = ctx.getPhysicalDevice();
    initInfo.Device          = m_device;
    initInfo.QueueFamily     = indices.graphicsFamily.value();
    initInfo.Queue           = ctx.getGraphicsQueue();
    initInfo.DescriptorPool  = m_pool;
    initInfo.MinImageCount   = sc.getImageCount();
    initInfo.ImageCount      = sc.getImageCount();
    initInfo.PipelineCache   = VK_NULL_HANDLE;
    initInfo.Allocator       = nullptr;
    initInfo.CheckVkResultFn = nullptr;
    initInfo.UseDynamicRendering = false;

    initInfo.PipelineInfoMain.RenderPass  = m_renderPass;
    initInfo.PipelineInfoMain.Subpass     = 0;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&initInfo))
        throw std::runtime_error("failed to init ImGui Vulkan backend!");
}

void ImGuiLayer::shutdown()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    for (auto fb : m_framebuffers)
        vkDestroyFramebuffer(m_device, fb, nullptr);
    m_framebuffers.clear();

    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    if (m_pool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

void ImGuiLayer::beginFrame()
{
    fprintf(stderr, "[ImGuiLayer] Before VulkanNewFrame\n");
    ImGui_ImplVulkan_NewFrame();
    fprintf(stderr, "[ImGuiLayer] After VulkanNewFrame\n");
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void ImGuiLayer::endFrame(VkCommandBuffer cmd, uint32_t imageIndex)
{
    ImGui::Render();

    VkClearValue clearColor = {{{0.15f, 0.15f, 0.15f, 1.0f}}};

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = m_renderPass;
    rpInfo.framebuffer       = m_framebuffers[imageIndex];
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = m_extent;
    rpInfo.clearValueCount   = 1;
    rpInfo.pClearValues      = &clearColor;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
    auto* drawData = ImGui::GetDrawData();
    fprintf(stderr, "[ImGuiLayer] RenderDrawData: valid=%d cmds=%d vtx=%d idx=%d textures=%d\n",
            drawData->Valid, drawData->CmdListsCount, drawData->TotalVtxCount, drawData->TotalIdxCount,
            drawData->Textures ? (int)drawData->Textures->Size : -1);
    ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
    fprintf(stderr, "[ImGuiLayer] After RenderDrawData\n");
    vkCmdEndRenderPass(cmd);
}

void ImGuiLayer::enableDocking()
{
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
}

void ImGuiLayer::onSwapChainRecreated(Renderer& renderer)
{
    SwapChain& sc = renderer.getSwapChain();
    m_extent = sc.getExtent();

    // Recreate framebuffers
    for (auto fb : m_framebuffers)
        vkDestroyFramebuffer(m_device, fb, nullptr);
    m_framebuffers.clear();

    createFramebuffers(m_device, sc.getImageViews(), sc.getExtent());
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void ImGuiLayer::createRenderPass(VkDevice device, VkFormat imageFormat)
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = imageFormat;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;  // clear — scene renders to offscreen
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &colorAttachment;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies   = &dependency;

    if (vkCreateRenderPass(device, &rpInfo, nullptr, &m_renderPass) != VK_SUCCESS)
        throw std::runtime_error("failed to create ImGui render pass!");
}

void ImGuiLayer::createFramebuffers(VkDevice device,
                                    const std::vector<VkImageView>& imageViews,
                                    VkExtent2D extent)
{
    m_framebuffers.resize(imageViews.size());

    for (size_t i = 0; i < imageViews.size(); i++)
    {
        VkImageView attachments[] = { imageViews[i] };

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = m_renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = attachments;
        fbInfo.width           = extent.width;
        fbInfo.height          = extent.height;
        fbInfo.layers          = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
            throw std::runtime_error("failed to create ImGui framebuffer!");
    }
}

} // namespace QymEngine
