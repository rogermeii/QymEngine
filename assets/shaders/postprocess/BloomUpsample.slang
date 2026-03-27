// Tent filter 升采样 + 合并当前级降采样数据
// binding 0: 更深一级的 mip（升采样源，来自 up image 或最深级的 down image）
// binding 1: 当前级的 downsample 数据（来自 down image，要保留/合并的）

[[vk::binding(0, 0)]] Sampler2D deeperMip;
[[vk::binding(1, 0)]] Sampler2D currentDownMip;

struct BloomPushConstant {
    float2 texelSize;
    float threshold;
    float intensity;
    int mipLevel;
    int useBrightPass;
};

[[vk::push_constant]] BloomPushConstant pc;

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

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 ts = pc.texelSize;

    // 3x3 tent filter（从更深 mip 升采样）
    float3 upsampled = float3(0, 0, 0);
    upsampled += deeperMip.Sample(uv + float2(-ts.x, ts.y)).rgb;
    upsampled += deeperMip.Sample(uv + float2(0, ts.y)).rgb * 2.0;
    upsampled += deeperMip.Sample(uv + float2(ts.x, ts.y)).rgb;

    upsampled += deeperMip.Sample(uv + float2(-ts.x, 0)).rgb * 2.0;
    upsampled += deeperMip.Sample(uv).rgb * 4.0;
    upsampled += deeperMip.Sample(uv + float2(ts.x, 0)).rgb * 2.0;

    upsampled += deeperMip.Sample(uv + float2(-ts.x, -ts.y)).rgb;
    upsampled += deeperMip.Sample(uv + float2(0, -ts.y)).rgb * 2.0;
    upsampled += deeperMip.Sample(uv + float2(ts.x, -ts.y)).rgb;

    // tent filter 权重归一化
    upsampled /= 16.0;

    // 读取当前级的 downsample 数据并相加（替代硬件加法混合）
    float3 existing = currentDownMip.Sample(uv).rgb;

    return float4(existing + upsampled, 1.0);
}
