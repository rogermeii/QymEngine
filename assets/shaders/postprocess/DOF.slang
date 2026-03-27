// Depth of Field (景深) — 单 pass Poisson Disc 模糊

[[vk::binding(0, 0)]] Sampler2D sceneHDR;
[[vk::binding(1, 0)]] Sampler2D depthTex;

struct DOFPushConstant {
    float2 texelSize;
    float focalDistance;
    float focalRange;
    float maxBlur;
    int autoFocus;
    float nearPlane;
    float farPlane;
};

[[vk::push_constant]] DOFPushConstant pc;

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
#ifdef GRAPHICS_FLIP_Y
    o.uv.y = 1.0 - o.uv.y;
#endif
    return o;
}

// 将 [0,1] 深度线性化为视空间距离
float linearizeDepth(float d) {
    return pc.nearPlane * pc.farPlane / (pc.farPlane - d * (pc.farPlane - pc.nearPlane));
}

// 16 点 Poisson Disc 采样分布
static const float2 poissonDisk[16] = {
    float2(-0.94201624, -0.39906216), float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845), float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554), float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507), float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367), float2( 0.14383161, -0.14100790)
};

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float2 uv = input.uv;

    // 确定对焦距离（自动或手动）
    float focal = pc.focalDistance;
    if (pc.autoFocus != 0) {
        float centerDepth = depthTex.Sample(float2(0.5, 0.5)).r;
        focal = linearizeDepth(centerDepth);
    }

    // 当前像素深度
    float depth = linearizeDepth(depthTex.Sample(uv).r);

    // 计算 CoC: 离焦平面的距离
    float coc = abs(depth - focal) / pc.focalRange;
    coc = saturate(coc);
    float blurRadius = coc * pc.maxBlur;

    if (blurRadius < 0.5) {
        // 在焦点范围内，无需模糊
        return float4(sceneHDR.Sample(uv).rgb, 1.0);
    }

    // Poisson Disc 模糊
    float3 color = float3(0, 0, 0);
    float totalWeight = 0.0;
    for (int i = 0; i < 16; i++) {
        float2 offset = poissonDisk[i] * blurRadius * pc.texelSize;
        float3 sampleColor = sceneHDR.Sample(uv + offset).rgb;

        // 根据采样点自身的 CoC 加权，减少前后景泄漏
        float sampleDepth = linearizeDepth(depthTex.Sample(uv + offset).r);
        float sampleCoc = saturate(abs(sampleDepth - focal) / pc.focalRange);
        float weight = max(sampleCoc, coc);

        color += sampleColor * weight;
        totalWeight += weight;
    }
    color /= max(totalWeight, 0.001);

    return float4(color, 1.0);
}
