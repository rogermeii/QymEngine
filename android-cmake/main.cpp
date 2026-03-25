#include "core/Application.h"
#include "core/Window.h"
#include "renderer/Renderer.h"
#include "renderer/VkDispatch.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "VirtualJoystick.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>

#include <SDL.h>
#include <cstring>

// Forward declare EditorApp (from editor/)
namespace QymEngine { class EditorApp; }
#include "EditorApp.h"

// ============================================================
// Runtime Player: fullscreen scene + dual virtual joysticks
// ============================================================
class RuntimeApp : public QymEngine::Application {
public:
    RuntimeApp() : Application({"QymEngine", 0, 0, true}) {}

protected:
    void onInit() override {
        m_renderer.init(getWindow());
        m_renderer.setCamera(&m_camera);

        m_scene.deserialize("scenes/default.json");

        m_camera.target = {0.0f, 0.0f, 0.0f};
        m_camera.distance = 8.0f;
        m_camera.yaw = -45.0f;
        m_camera.pitch = 30.0f;

        // Init minimal ImGui for joystick drawing
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

        ImGui_ImplSDL2_InitForVulkan(getWindow().getNativeWindow());

        auto& ctx = m_renderer.getContext();
        auto& sc = m_renderer.getSwapChain();

        // Descriptor pool for ImGui
        VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10 };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 10;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        vkCreateDescriptorPool(ctx.getDevice(), &poolInfo, nullptr, &m_imguiPool);

        // Render pass for ImGui (renders on top of blit result)
        VkAttachmentDescription colorAtt{};
        colorAtt.format = sc.getImageFormat();
        colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // preserve blit content
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        colorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAtt;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dep;
        vkCreateRenderPass(ctx.getDevice(), &rpInfo, nullptr, &m_imguiRenderPass);

        // ImGui Vulkan init
        QymEngine::QueueFamilyIndices indices = ctx.findQueueFamilies(ctx.getPhysicalDevice());
        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = ctx.getInstance();
        initInfo.PhysicalDevice = ctx.getPhysicalDevice();
        initInfo.Device = ctx.getDevice();
        initInfo.QueueFamily = indices.graphicsFamily.value();
        initInfo.Queue = ctx.getGraphicsQueue();
        initInfo.DescriptorPool = m_imguiPool;
        initInfo.MinImageCount = sc.getImageCount();
        initInfo.ImageCount = sc.getImageCount();
        initInfo.PipelineInfoMain.RenderPass = m_imguiRenderPass;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_Init(&initInfo);

        // Register event callback for ImGui + joysticks
        getWindow().setEventCallback([this](const SDL_Event& event) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            // Joysticks consume touch events
            m_leftJoystick.processEvent(event);
            m_rightJoystick.processEvent(event);
        });

        recreateFramebuffers();
    }

    void onUpdate() override {
        // Sync offscreen to swapchain BEFORE beginFrame to avoid
        // vkDeviceWaitIdle conflicting with in-flight command buffer
        VkExtent2D extent = m_renderer.getSwapChain().getExtent();
        if (extent.width > 0 && extent.height > 0) {
            if (!m_renderer.isOffscreenReady() ||
                m_renderer.getOffscreenWidth() != extent.width ||
                m_renderer.getOffscreenHeight() != extent.height) {
                vkDeviceWaitIdle(m_renderer.getContext().getDevice());
                m_renderer.resizeOffscreen(extent.width, extent.height);
                recreateFramebuffers();
            }
        }

        // Update joystick positions
        float sw = static_cast<float>(extent.width);
        float sh = static_cast<float>(extent.height);
        float joyRadius = sh * 0.18f;
        float margin = joyRadius * 1.5f;
        m_leftJoystick.setScreenSize(sw, sh);
        m_leftJoystick.setPosition(margin, sh - margin, joyRadius);
        m_rightJoystick.setScreenSize(sw, sh);
        m_rightJoystick.setPosition(sw - margin, sh - margin, joyRadius);

        // Apply joystick input to camera
        float dt = ImGui::GetIO().DeltaTime;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;

        glm::vec2 moveDir = m_leftJoystick.getDirection();
        if (glm::length(moveDir) > 0.05f) {
            float speed = 3.0f * dt;
            glm::vec3 forward = glm::normalize(m_camera.target - m_camera.getPosition());
            glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
            m_camera.target += forward * (-moveDir.y * speed) + right * (moveDir.x * speed);
        }

        glm::vec2 orbitDir = m_rightJoystick.getDirection();
        if (glm::length(orbitDir) > 0.05f) {
            m_camera.orbit(orbitDir.x * 2.0f, orbitDir.y * 2.0f);
        }

        if (!m_renderer.isOffscreenReady())
            return;

        if (m_renderer.beginFrame()) {
            m_renderer.drawScene(m_scene);
            m_renderer.blitToSwapchain();

            // Draw joystick overlay via ImGui
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();

            m_leftJoystick.draw();
            m_rightJoystick.draw();

            ImGui::Render();

            // Render ImGui overlay on swapchain
            VkCommandBuffer cmd = m_renderer.getCurrentCommandBuffer();
            uint32_t imgIdx = m_renderer.getImageIndex();

            VkRenderPassBeginInfo rpBegin{};
            rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpBegin.renderPass = m_imguiRenderPass;
            rpBegin.framebuffer = m_imguiFBs[imgIdx];
            rpBegin.renderArea.extent = m_renderer.getSwapChain().getExtent();
            vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            vkCmdEndRenderPass(cmd);

            m_renderer.endFrame();
        }
    }

    void onShutdown() override {
        VkDevice device = m_renderer.getContext().getDevice();
        vkDeviceWaitIdle(device);

        for (auto fb : m_imguiFBs)
            vkDestroyFramebuffer(device, fb, nullptr);
        m_imguiFBs.clear();

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();

        if (m_imguiRenderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(device, m_imguiRenderPass, nullptr);
        if (m_imguiPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, m_imguiPool, nullptr);

        m_renderer.shutdown();
    }

private:
    void recreateFramebuffers() {
        VkDevice device = m_renderer.getContext().getDevice();
        for (auto fb : m_imguiFBs)
            vkDestroyFramebuffer(device, fb, nullptr);

        auto& sc = m_renderer.getSwapChain();
        auto& views = sc.getImageViews();
        m_imguiFBs.resize(views.size());

        for (size_t i = 0; i < views.size(); i++) {
            VkImageView att = views[i];
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = m_imguiRenderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &att;
            fbInfo.width = sc.getExtent().width;
            fbInfo.height = sc.getExtent().height;
            fbInfo.layers = 1;
            vkCreateFramebuffer(device, &fbInfo, nullptr, &m_imguiFBs[i]);
        }
    }

    QymEngine::Renderer m_renderer;
    QymEngine::Scene    m_scene;
    QymEngine::Camera   m_camera;

    QymEngine::VirtualJoystick m_leftJoystick;
    QymEngine::VirtualJoystick m_rightJoystick;

    VkDescriptorPool m_imguiPool = VK_NULL_HANDLE;
    VkRenderPass m_imguiRenderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_imguiFBs;
};

// ============================================================
// Main: dispatch based on arguments
// ============================================================
int main(int argc, char* argv[]) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint("SDL_ANDROID_HIDE_SYSTEM_BARS", "1");

    bool runtimePlayer = false;
    bool useGLES = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-runtime-player") == 0)
            runtimePlayer = true;
        if (strcmp(argv[i], "-gles") == 0 || strcmp(argv[i], "--gles") == 0)
            useGLES = true;
    }

    // 确保 SDL 视频子系统初始化 (vkInitDispatch 需要 SDL 来加载 Vulkan)
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("[QymEngine] SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Android: 默认使用 Vulkan，可通过 -gles/--gles 参数切换到 GLES 后端
    if (useGLES) {
        QymEngine::vkInitDispatch(4); // GLES backend
        SDL_Log("[QymEngine] Using GLES backend");
    } else {
        QymEngine::vkInitDispatch(0); // Vulkan backend
        SDL_Log("[QymEngine] Using Vulkan backend");
    }

    try {
        if (runtimePlayer) {
            RuntimeApp app;
            app.run();
        } else {
            auto backend = useGLES ? QymEngine::RenderBackend::GLES : QymEngine::RenderBackend::Vulkan;
            QymEngine::EditorApp app(backend);
            app.run();
        }
    } catch (const std::exception& e) {
        SDL_Log("QymEngine crashed: %s", e.what());
        return 1;
    } catch (...) {
        SDL_Log("QymEngine crashed: unknown exception");
        return 1;
    }
    return 0;
}
