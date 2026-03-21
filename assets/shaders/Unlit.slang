// Unlit shader (flat color / texture)

// Set 0: per-frame (engine-managed, all shaders must match)
struct FrameData
{
    float4x4 view;
    float4x4 proj;
    float3 lightDir;
    float3 lightColor;
    float3 ambientColor;
    float3 cameraPos;
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;

// Set 1: per-material (shader-specific)
struct UnlitMaterialParams
{
    float4 baseColor;
};

[[vk::binding(0, 1)]] ConstantBuffer<UnlitMaterialParams> materialParams;
[[vk::binding(1, 1)]] Sampler2D albedoMap;

// Push constant: per-object
struct PushConstants
{
    float4x4 model;
    int highlighted;
};

[[vk::push_constant]] PushConstants pc;

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
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(pc.model, float4(input.position, 1.0));
    output.sv_position = mul(frame.proj, mul(frame.view, worldPos));
    output.color = float3(0.0);
    output.texCoord = input.texCoord;
    output.highlighted = pc.highlighted;
    output.normal = float3(0.0, 1.0, 0.0);
    output.worldPos = float3(0.0);
    output.baseColor = materialParams.baseColor;
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
