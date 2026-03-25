// ============================================================================
// SPIRV-Cross 封装 — 纯 C++ 编译单元（无 ARC、无 ObjC）
//
// 将 SPIRV-Cross 调用从 VkMetal.mm（-fobjc-arc）中分离出来，
// 避免 Objective-C ARC 干扰 SPIRV-Cross 内部 C++ 对象的生命周期管理。
// 同时在独立大栈线程上运行，防止 iOS 主线程栈空间不足。
// ============================================================================

#include "renderer/metal/SpirvToMSL.h"
#include "renderer/metal/VkMetalHandles.h"
#include <SDL.h>
#include <memory>
#include <pthread.h>
#include <functional>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// SPIRV-Cross 头文件
#if __has_include(<spirv_cross/spirv_msl.hpp>)
#include <spirv_cross/spirv_msl.hpp>
#else
#include <spirv_msl.hpp>
#endif

// iOS 主线程栈仅约 1MB，SPIRV-Cross compile() 可能需要更多栈空间。
// 此辅助函数检测剩余栈空间，不足时在大栈线程上执行。
static void runOnLargeStack(std::function<void()> fn, size_t stackSize = 8 * 1024 * 1024)
{
    pthread_t self = pthread_self();
    void* stackAddr = pthread_get_stackaddr_np(self);
    size_t currentStackSize = pthread_get_stacksize_np(self);
    volatile char dummy;
    size_t stackUsed = (uintptr_t)stackAddr - (uintptr_t)&dummy;
    size_t stackFree = currentStackSize > stackUsed ? currentStackSize - stackUsed : 0;

    // 栈空间充足（>4MB），直接执行
    if (stackFree > 4 * 1024 * 1024) {
        fn();
        return;
    }

    // 在大栈工作线程上执行
    std::exception_ptr exPtr;
    pthread_t thread;
    pthread_attr_t attr;
    struct ThreadArgs {
        std::function<void()>* fn;
        std::exception_ptr* exPtr;
    };
    ThreadArgs args{&fn, &exPtr};

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackSize);

    auto threadFunc = [](void* arg) -> void* {
        auto* a = static_cast<ThreadArgs*>(arg);
        try {
            (*a->fn)();
        } catch (...) {
            *a->exPtr = std::current_exception();
        }
        return nullptr;
    };

    pthread_create(&thread, &attr, threadFunc, &args);
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);

    if (exPtr) std::rethrow_exception(exPtr);
}

void compileSpirvToMSL(std::vector<uint32_t> spirvData, MTL_ShaderModule* mod)
{
    if (!mod || spirvData.empty()) return;

    runOnLargeStack([&spirvData, mod]() {
    try {
        // 在堆上创建 CompilerMSL（对象较大，避免栈压力）
        auto compiler = std::make_unique<spirv_cross::CompilerMSL>(std::move(spirvData));

        // 配置 MSL 选项
        spirv_cross::CompilerMSL::Options mslOpts;
        mslOpts.set_msl_version(2, 0);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
        mslOpts.platform = spirv_cross::CompilerMSL::Options::iOS;
#else
        mslOpts.platform = spirv_cross::CompilerMSL::Options::macOS;
#endif
        compiler->set_msl_options(mslOpts);

        // 不做 flip_vert_y — Vulkan/Metal 的 framebuffer 坐标系 Y 都是从上到下
        // 翻转由 mtl_vkCmdBlitImage 在最终 blit 到 swapchain 时处理

        // 编译 SPIR-V → MSL
        mod->mslSource = compiler->compile();

        // 获取 entry point 信息
        auto entryPoints = compiler->get_entry_points_and_stages();
        if (!entryPoints.empty()) {
            auto& ep = entryPoints[0];
            mod->executionModel = (uint32_t)ep.execution_model;
            mod->entryPointName = compiler->get_cleansed_entry_point_name(
                ep.name, ep.execution_model);
            SDL_Log("[VkMetal]   entry point: '%s' (model=%u)",
                    mod->entryPointName.c_str(), mod->executionModel);
        }

        // 查询 SPIRV-Cross 分配的 Metal 资源绑定
        auto resources = compiler->get_shader_resources();

        // 顶点属性 (stage_inputs) — 仅顶点着色器有
        if (!resources.stage_inputs.empty() &&
            mod->executionModel == (uint32_t)spv::ExecutionModelVertex) {
            mod->bindings.hasVertexInputs = true;
        }

        // Uniform buffers
        for (auto& ub : resources.uniform_buffers) {
            uint32_t set = compiler->get_decoration(ub.id, spv::DecorationDescriptorSet);
            uint32_t binding = compiler->get_decoration(ub.id, spv::DecorationBinding);
            uint32_t mslBuf = compiler->get_automatic_msl_resource_binding(ub.id);
            mod->bindings.uboBindings[{set, binding}] = mslBuf;
            SDL_Log("[VkMetal]   UBO set=%u bind=%u -> Metal buffer(%u)", set, binding, mslBuf);
        }

        // Combined image samplers
        for (auto& img : resources.sampled_images) {
            uint32_t set = compiler->get_decoration(img.id, spv::DecorationDescriptorSet);
            uint32_t binding = compiler->get_decoration(img.id, spv::DecorationBinding);
            uint32_t mslTex = compiler->get_automatic_msl_resource_binding(img.id);
            uint32_t mslSmp = compiler->get_automatic_msl_resource_binding_secondary(img.id);
            mod->bindings.textureBindings[{set, binding}] = mslTex;
            mod->bindings.samplerBindings[{set, binding}] = mslSmp;
            SDL_Log("[VkMetal]   Texture set=%u bind=%u -> Metal texture(%u) sampler(%u)",
                    set, binding, mslTex, mslSmp);
        }

        // Separate images
        for (auto& img : resources.separate_images) {
            uint32_t set = compiler->get_decoration(img.id, spv::DecorationDescriptorSet);
            uint32_t binding = compiler->get_decoration(img.id, spv::DecorationBinding);
            uint32_t mslTex = compiler->get_automatic_msl_resource_binding(img.id);
            mod->bindings.textureBindings[{set, binding}] = mslTex;
        }

        // Separate samplers
        for (auto& smp : resources.separate_samplers) {
            uint32_t set = compiler->get_decoration(smp.id, spv::DecorationDescriptorSet);
            uint32_t binding = compiler->get_decoration(smp.id, spv::DecorationBinding);
            uint32_t mslSmp = compiler->get_automatic_msl_resource_binding(smp.id);
            mod->bindings.samplerBindings[{set, binding}] = mslSmp;
        }

        // Push constants
        if (!resources.push_constant_buffers.empty()) {
            uint32_t pcBuf = compiler->get_automatic_msl_resource_binding(
                resources.push_constant_buffers[0].id);
            mod->bindings.pushConstantBufferIndex = pcBuf;
            SDL_Log("[VkMetal]   Push constants -> Metal buffer(%u)", pcBuf);
        }

        // 计算顶点缓冲的 Metal buffer index:
        // 使用所有已分配 buffer index 的最大值 + 1
        if (mod->bindings.hasVertexInputs) {
            uint32_t maxBufIdx = 0;
            for (auto& [key, idx] : mod->bindings.uboBindings) {
                if (idx != UINT32_MAX && idx >= maxBufIdx)
                    maxBufIdx = idx + 1;
            }
            if (mod->bindings.pushConstantBufferIndex != UINT32_MAX &&
                mod->bindings.pushConstantBufferIndex >= maxBufIdx) {
                maxBufIdx = mod->bindings.pushConstantBufferIndex + 1;
            }
            mod->bindings.vertexBufferIndex = maxBufIdx;
            SDL_Log("[VkMetal]   vertex buffer -> Metal buffer(%u)", maxBufIdx);
        }

        SDL_Log("[VkMetal] SPIR-V -> MSL OK (%zu chars)", mod->mslSource.size());

    } catch (const spirv_cross::CompilerError& e) {
        SDL_Log("[VkMetal] SPIR-V -> MSL failed: %s", e.what());
    } catch (const std::exception& e) {
        SDL_Log("[VkMetal] SPIR-V -> MSL exception: %s", e.what());
    }
    }); // runOnLargeStack
}
