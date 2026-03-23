// Triangle/Basic shader (Blinn-Phong lighting, multi-light)

static const int LIGHT_DIRECTIONAL = 0;
static const int LIGHT_POINT       = 1;
static const int LIGHT_SPOT        = 2;
static const int MAX_LIGHTS        = 8;

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
    [[vk::location(6)]] float metallic : TEXCOORD3;
    [[vk::location(7)]] float roughness : TEXCOORD4;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(pc.model, float4(input.position, 1.0));
    output.sv_position = mul(frame.proj, mul(frame.view, worldPos));
    output.color = input.color;
    output.texCoord = input.texCoord;
    output.highlighted = float(pc.highlighted);
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
    if (input.highlighted > 0.5)
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
    float3 mapN = normalMap.Sample(input.texCoord).rgb * 2.0 - 1.0;
#endif
    float3 normal = normalize(input.normal + mapN * 0.0001);
    float3 viewDir = normalize(frame.cameraPos - input.worldPos);

    float shadowFactor = 1.0;
    if (frame.shadowParams.x != 0) {
        float4 lightClip = mul(frame.lightVP, float4(input.worldPos, 1.0));
        float3 lightNDC = lightClip.xyz / lightClip.w;
        float2 shadowUV = lightNDC.xy * 0.5 + 0.5;
        if (frame.shadowParams.y != 0) shadowUV.y = 1.0 - shadowUV.y;
        float currentDepth = lightNDC.z;
        if (frame.shadowParams.z != 0)
            currentDepth = currentDepth * 0.5 + 0.5;

        if (shadowUV.x >= 0.0 && shadowUV.x <= 1.0 && shadowUV.y >= 0.0 && shadowUV.y <= 1.0) {
            float shadow = 0.0;
            float2 texelSize = float2(1.0 / 2048.0, 1.0 / 2048.0);
            float bias = 0.005;
            for (int sx = -1; sx <= 1; sx++) {
                for (int sy = -1; sy <= 1; sy++) {
                    float closestDepth = shadowMap.Sample(shadowUV + float2(sx, sy) * texelSize).r;
                    shadow += (currentDepth - bias > closestDepth) ? 0.0 : 1.0;
                }
            }
            shadowFactor = shadow / 9.0;
        }
    }

    float3 ambient = frame.ambientColor * albedo;
    float3 Lo = float3(0.0);

    int lightCount = frame.lightCountPad.x;
    bool shadowApplied = false;
    for (int i = 0; i < lightCount && i < MAX_LIGHTS; i++) {
        LightData light = frame.lights[i];
        int lightType = int(light.positionAndType.w);
        float3 lightColor = light.colorAndIntensity.xyz;
        float3 L;
        float atten = 1.0;

        if (lightType == LIGHT_DIRECTIONAL) {
            L = normalize(-light.directionAndRange.xyz);
            if (!shadowApplied) { atten *= shadowFactor; shadowApplied = true; }
        } else {
            float3 toLight = light.positionAndType.xyz - input.worldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            float range = light.directionAndRange.w;
            float distRatio = saturate(1.0 - pow(dist / range, 4.0));
            atten = distRatio * distRatio / (dist * dist + 1.0);
            if (lightType == LIGHT_SPOT) {
                float cosAngle = dot(-L, normalize(light.directionAndRange.xyz));
                atten *= saturate((cosAngle - light.spotParams.y) / max(light.spotParams.x - light.spotParams.y, 0.0001));
            }
        }

        float diff = max(dot(normal, L), 0.0);
        float3 halfDir = normalize(L + viewDir);
        float shininess = lerp(256.0, 8.0, input.roughness);
        float spec = pow(max(dot(normal, halfDir), 0.0), shininess);
        float specStrength = lerp(0.3, 1.0, input.metallic);

        Lo += (diff * albedo * (1.0 - input.metallic) + spec * specStrength) * lightColor * atten;
    }

    float3 result = ambient + Lo;
    return float4(result, input.baseColor.a);
}
