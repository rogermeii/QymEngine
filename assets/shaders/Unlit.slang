// Unlit shader (flat color / texture)

static const int MAX_LIGHTS = 8;

struct LightData
{
    float4 positionAndType;
    float4 directionAndRange;
    float4 colorAndIntensity;
    float4 spotParams;
};

// Set 0: per-frame (engine-managed, all shaders must match)
struct FrameData
{
    float4x4 view;
    float4x4 proj;
    float3 ambientColor;
    float3 cameraPos;
    int4 lightCountPad;
    LightData lights[MAX_LIGHTS];
    float4x4 lightVP;
    int4 shadowParams;
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] Sampler2D shadowMap;

#ifdef USE_BINDLESS
// --- Bindless path (PC) ---
[[vk::binding(0, 1)]] SamplerState bindlessSamplers[256];
[[vk::binding(1, 1)]] Texture2D bindlessTextures[];
struct MaterialEntry
{
    float4 baseColor;
    float metallic;
    float roughness;
    uint albedoTexIndex;
    uint normalTexIndex;
    uint samplerIndex;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};
[[vk::binding(2, 1)]] StructuredBuffer<MaterialEntry> materialEntries;

struct PushConstants
{
    float4x4 model;
    int highlighted;
    uint materialIndex;
};

#else
// --- Non-bindless path (current) ---
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
#endif

[[vk::push_constant]] PushConstants pc;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(3)]] float3 normal : NORMAL;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float3 color : COLOR;
    [[vk::location(1)]] float2 texCoord : TEXCOORD0;
    [[vk::location(2)]] float highlighted : BLENDINDICES;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(4)]] float3 worldPos : TEXCOORD1;
    [[vk::location(5)]] float4 baseColor : TEXCOORD2;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(pc.model, float4(input.position, 1.0));
    output.sv_position = mul(frame.proj, mul(frame.view, worldPos));
    output.color = float3(0.0);
    output.texCoord = input.texCoord;
    output.highlighted = float(pc.highlighted);
    output.normal = float3(0.0, 1.0, 0.0);
    output.worldPos = float3(0.0);
#ifdef USE_BINDLESS
    MaterialEntry entry = materialEntries[pc.materialIndex];
    output.baseColor = entry.baseColor;
#else
    output.baseColor = materialParams.baseColor;
#endif
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    if (input.highlighted > 0.5)
        return input.baseColor;

#ifdef USE_BINDLESS
    MaterialEntry entry = materialEntries[pc.materialIndex];
    float4 texColor = bindlessTextures[NonUniformResourceIndex(entry.albedoTexIndex)]
                      .Sample(bindlessSamplers[NonUniformResourceIndex(entry.samplerIndex)], input.texCoord);
    return texColor * input.baseColor;
#else
    float4 texColor = albedoMap.Sample(input.texCoord);
    return texColor * input.baseColor;
#endif
}
