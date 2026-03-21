// Unlit shader (flat color / texture)

struct UBO
{
    float4x4 view;
    float4x4 proj;
    float3 lightDir;
    float3 lightColor;
    float3 ambientColor;
    float3 cameraPos;
};

struct PushConstants
{
    float4x4 model;
    float4 baseColor;
    float metallic;
    float roughness;
    int highlighted;
};

[[vk::binding(0, 0)]] ConstantBuffer<UBO> ubo;
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 1)]] Sampler2D albedoMap;

struct VSInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 color;
    [[vk::location(2)]] float2 texCoord;
    [[vk::location(3)]] float3 normal;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float2 texCoord;
    [[vk::location(2)]] nointerpolation int highlighted;
    [[vk::location(3)]] float3 normal;
    [[vk::location(4)]] float3 worldPos;
    [[vk::location(5)]] float4 baseColor;
    [[vk::location(6)]] float metallic;
    [[vk::location(7)]] float roughness;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(pc.model, float4(input.position, 1.0));
    output.sv_position = mul(ubo.proj, mul(ubo.view, worldPos));
    output.color = float3(0.0);
    output.texCoord = input.texCoord;
    output.highlighted = pc.highlighted;
    output.normal = float3(0.0, 1.0, 0.0);
    output.worldPos = float3(0.0);
    output.baseColor = pc.baseColor;
    output.metallic = 0.0;
    output.roughness = 0.0;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    if (input.highlighted == 1)
        return input.baseColor;

    float4 texColor = albedoMap.Sample(input.texCoord);
    return texColor * input.baseColor;
}
