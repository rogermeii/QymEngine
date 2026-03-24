// 13-tap downsample filter (CoD 2014) + 可选 bright extract

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
    o.uv.y = 1.0 - o.uv.y;
    return o;
}

float3 brightPass(float3 color) {
    float br = max(color.r, max(color.g, color.b));
    float soft = br - pc.threshold + 0.5;
    soft = clamp(soft, 0.0, 1.0);
    soft = soft * soft;
    float contribution = max(soft, br - pc.threshold) / max(br, 0.00001);
    return color * max(contribution, 0.0);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 ts = pc.texelSize;

    // 13-tap downsample
    float3 a = inputTex.Sample(uv + float2(-2*ts.x, 2*ts.y)).rgb;
    float3 b = inputTex.Sample(uv + float2(0, 2*ts.y)).rgb;
    float3 c = inputTex.Sample(uv + float2(2*ts.x, 2*ts.y)).rgb;

    float3 d = inputTex.Sample(uv + float2(-2*ts.x, 0)).rgb;
    float3 e = inputTex.Sample(uv).rgb;
    float3 f = inputTex.Sample(uv + float2(2*ts.x, 0)).rgb;

    float3 g = inputTex.Sample(uv + float2(-2*ts.x, -2*ts.y)).rgb;
    float3 h = inputTex.Sample(uv + float2(0, -2*ts.y)).rgb;
    float3 i = inputTex.Sample(uv + float2(2*ts.x, -2*ts.y)).rgb;

    float3 j = inputTex.Sample(uv + float2(-ts.x, ts.y)).rgb;
    float3 k = inputTex.Sample(uv + float2(ts.x, ts.y)).rgb;
    float3 l = inputTex.Sample(uv + float2(-ts.x, -ts.y)).rgb;
    float3 m = inputTex.Sample(uv + float2(ts.x, -ts.y)).rgb;

    float3 color = e * 0.125;
    color += (a + c + g + i) * 0.03125;
    color += (b + d + f + h) * 0.0625;
    color += (j + k + l + m) * 0.125;

    if (pc.useBrightPass != 0) {
        color = brightPass(color);
    }

    return float4(color, 1.0);
}
