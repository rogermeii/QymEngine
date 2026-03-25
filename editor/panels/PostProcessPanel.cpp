#include "PostProcessPanel.h"
#include <imgui.h>
#include "renderer/PostProcess.h"

namespace QymEngine {

void PostProcessPanel::onImGuiRender(Scene& scene) {
    ImGui::Begin("后处理");
    auto& pp = scene.getPostProcessSettings();

    if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用##bloom", &pp.bloomEnabled);
        if (pp.bloomEnabled) {
            ImGui::DragFloat("阈值", &pp.bloomThreshold, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("强度##bloom", &pp.bloomIntensity, 0.01f, 0.0f, 5.0f);
            ImGui::SliderInt("Mip 级数", &pp.bloomMipCount, 1, MAX_BLOOM_MIPS);
        }
    }

    if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用##tonemapping", &pp.toneMappingEnabled);
        ImGui::Checkbox("自动曝光", &pp.autoExposureEnabled);
        if (pp.autoExposureEnabled) {
            ImGui::DragFloat("最小曝光", &pp.autoExposureMin, 0.01f, 0.01f, 10.0f);
            ImGui::DragFloat("最大曝光", &pp.autoExposureMax, 0.01f, 0.1f, 20.0f);
        } else {
            ImGui::DragFloat("曝光", &pp.exposure, 0.01f, 0.001f, 20.0f);
        }
    }

    if (ImGui::CollapsingHeader("Color Grading", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用##colorgrading", &pp.colorGradingEnabled);
        if (pp.colorGradingEnabled) {
            ImGui::SliderFloat("对比度", &pp.contrast, 0.5f, 2.0f);
            ImGui::SliderFloat("饱和度", &pp.saturation, 0.0f, 2.0f);
            ImGui::SliderFloat("色温", &pp.temperature, -1.0f, 1.0f);
            ImGui::SliderFloat("色调", &pp.tint, -1.0f, 1.0f);
            ImGui::SliderFloat("亮度", &pp.brightness, -1.0f, 1.0f);
        }
    }

    if (ImGui::CollapsingHeader("Vignette", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用##vignette", &pp.vignetteEnabled);
        if (pp.vignetteEnabled) {
            ImGui::SliderFloat("强度##vignette", &pp.vignetteIntensity, 0.0f, 1.0f);
            ImGui::SliderFloat("平滑度", &pp.vignetteSmoothness, 0.01f, 2.0f);
        }
    }

    if (ImGui::CollapsingHeader("FXAA", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用##fxaa", &pp.fxaaEnabled);
        if (pp.fxaaEnabled) {
            ImGui::SliderFloat("子像素质量", &pp.fxaaSubpixQuality, 0.0f, 1.0f);
            ImGui::SliderFloat("边缘阈值", &pp.fxaaEdgeThreshold, 0.063f, 0.333f);
            ImGui::SliderFloat("最小阈值", &pp.fxaaEdgeThresholdMin, 0.0312f, 0.0833f);
        }
    }

    pp.clampValues();
    ImGui::End();
}

} // namespace QymEngine
