// Triangle/Basic shader (Blinn-Phong lighting)

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
struct TriangleMaterialParams
{
    float4 baseColor;
    float metallic;
    float roughness;
};

[[vk::binding(0, 1)]] ConstantBuffer<TriangleMaterialParams> materialParams;
[[vk::binding(1, 1)]] Sampler2D albedoMap;
[[vk::binding(2, 1)]] Sampler2D normalMap;

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
    output.sv_position = mul(frame.proj, mul(frame.view, worldPos));
    output.color = input.color;
    output.texCoord = input.texCoord;
    output.highlighted = pc.highlighted;
    float3x3 modelMat3 = (float3x3)pc.model;
    output.normal = mul(modelMat3, input.normal);
    output.worldPos = worldPos.xyz;
#ifdef USE_BINDLESS
    MaterialEntry entry = materialEntries[pc.materialIndex];
    output.baseColor = entry.baseColor;
    output.metallic = entry.metallic;
    output.roughness = entry.roughness;
#else
    output.baseColor = materialParams.baseColor;
    output.metallic = materialParams.metallic;
    output.roughness = materialParams.roughness;
#endif
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    if (input.highlighted == 1)
        return input.baseColor;

#ifdef USE_BINDLESS
    MaterialEntry entry = materialEntries[pc.materialIndex];
    float3 texColor = bindlessTextures[NonUniformResourceIndex(entry.albedoTexIndex)]
                      .Sample(bindlessSamplers[NonUniformResourceIndex(entry.samplerIndex)], input.texCoord).rgb;
    float3 albedo = texColor * input.baseColor.rgb;
    float3 mapN = bindlessTextures[NonUniformResourceIndex(entry.normalTexIndex)]
                  .Sample(bindlessSamplers[NonUniformResourceIndex(entry.samplerIndex)], input.texCoord).rgb * 2.0 - 1.0;
#else
    float3 texColor = albedoMap.Sample(input.texCoord).rgb;
    float3 albedo = texColor * input.baseColor.rgb;
    // Sample normal map to keep binding alive (trivial usage for layout compatibility)
    float3 mapN = normalMap.Sample(input.texCoord).rgb * 2.0 - 1.0;
#endif
    float3 normal = normalize(input.normal + mapN * 0.0001);
    float3 lightD = normalize(-frame.lightDir);

    float3 ambient = frame.ambientColor * albedo;

    float diff = max(dot(normal, lightD), 0.0);
    float3 diffuseColor = diff * frame.lightColor * albedo * (1.0 - input.metallic);

    float3 viewDir = normalize(frame.cameraPos - input.worldPos);
    float3 halfDir = normalize(lightD + viewDir);
    float shininess = lerp(256.0, 8.0, input.roughness);
    float spec = pow(max(dot(normal, halfDir), 0.0), shininess);
    float specStrength = lerp(0.3, 1.0, input.metallic);
    float3 specular = spec * frame.lightColor * specStrength;

    float3 result = ambient + diffuseColor + specular;
    return float4(result, input.baseColor.a);
}
