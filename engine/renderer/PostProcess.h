#pragma once
#include <algorithm>

namespace QymEngine {

static constexpr int MAX_BLOOM_MIPS = 6;

struct PostProcessSettings {
    // Bloom
    bool  bloomEnabled = true;
    float bloomThreshold = 1.0f;
    float bloomIntensity = 0.5f;
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

} // namespace QymEngine
