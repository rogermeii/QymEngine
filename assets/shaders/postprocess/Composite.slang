// Composite 后处理: Bloom 混合 + Tone Mapping (ACES) + Color Grading + Gamma 矫正

[[vk::binding(0, 0)]] Sampler2D sceneHDR;
[[vk::binding(1, 0)]] Sampler2D bloomTex;

struct CompositePushConstant {
    float2 texelSize;
    float exposure;
    float bloomIntensity;
    int bloomEnabled;
    int toneMappingEnabled;
    int colorGradingEnabled;
    float contrast;
    float saturation;
    float temperature;
    float tint;
    float brightness;
};

[[vk::push_constant]] CompositePushConstant pc;

struct VSOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

static const float3 positions[6] = {
    float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0),
    float3(-1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)
};

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID) {
    VSOutput o;
    float3 p = positions[vertexID];
    o.sv_position = float4(p, 1.0);
    o.uv = p.xy * 0.5 + 0.5;
    o.uv.y = 1.0 - o.uv.y;
    return o;
}

float3 ACESFilm(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 linearToSRGB(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return float3(
        c.x <= 0.0031308 ? lo.x : hi.x,
        c.y <= 0.0031308 ? lo.y : hi.y,
        c.z <= 0.0031308 ? lo.z : hi.z
    );
}

float3 adjustContrast(float3 color, float contrastVal) {
    float3 midgray = float3(0.5, 0.5, 0.5);
    return midgray + (color - midgray) * contrastVal;
}

float3 adjustSaturation(float3 color, float saturationVal) {
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    return lerp(float3(luma, luma, luma), color, saturationVal);
}

float3 adjustTemperature(float3 color, float temp, float tintVal) {
    color.r += temp * 0.1;
    color.b -= temp * 0.1;
    color.g -= tintVal * 0.1;
    return color;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float3 color = sceneHDR.Sample(input.uv).rgb;

    if (pc.bloomEnabled != 0) {
        float3 bloom = bloomTex.Sample(input.uv).rgb;
        color += bloom * pc.bloomIntensity;
    }

    color *= pc.exposure;

    if (pc.toneMappingEnabled != 0) {
        color = ACESFilm(color);
    } else {
        color = saturate(color);
    }

    if (pc.colorGradingEnabled != 0) {
        color = adjustContrast(color, pc.contrast);
        color = adjustSaturation(color, pc.saturation);
        color = adjustTemperature(color, pc.temperature, pc.tint);
        color += pc.brightness;
    }

    color = linearToSRGB(saturate(color));

    return float4(color, 1.0);
}
