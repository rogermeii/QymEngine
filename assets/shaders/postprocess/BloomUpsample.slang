// Tent filter 升采样

[[vk::binding(0, 0)]] Sampler2D inputTex;

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
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 ts = pc.texelSize;

    // 3x3 tent filter
    float3 color = float3(0, 0, 0);
    color += inputTex.Sample(uv + float2(-ts.x, ts.y)).rgb;
    color += inputTex.Sample(uv + float2(0, ts.y)).rgb * 2.0;
    color += inputTex.Sample(uv + float2(ts.x, ts.y)).rgb;

    color += inputTex.Sample(uv + float2(-ts.x, 0)).rgb * 2.0;
    color += inputTex.Sample(uv).rgb * 4.0;
    color += inputTex.Sample(uv + float2(ts.x, 0)).rgb * 2.0;

    color += inputTex.Sample(uv + float2(-ts.x, -ts.y)).rgb;
    color += inputTex.Sample(uv + float2(0, -ts.y)).rgb * 2.0;
    color += inputTex.Sample(uv + float2(ts.x, -ts.y)).rgb;

    // tent filter 权重归一化后乘以 intensity
    // intensity 由 C++ 侧设置为 1.0，bloom 强度在 Composite 中统一控制
    color /= 16.0;

    return float4(color, 1.0);
}
