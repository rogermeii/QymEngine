// Lit PBR shader (Cook-Torrance GGX, multi-light)

static const int LIGHT_DIRECTIONAL = 0;
static const int LIGHT_POINT       = 1;
static const int LIGHT_SPOT        = 2;
static const int MAX_LIGHTS        = 8;

struct LightData
{
    float4 positionAndType;     // xyz=position, w=type
    float4 directionAndRange;   // xyz=direction, w=range
    float4 colorAndIntensity;   // xyz=color*intensity, w=rawIntensity
    float4 spotParams;          // x=cosInnerAngle, y=cosOuterAngle
};

// Set 0: per-frame (engine-managed, all shaders must match)
struct FrameData
{
    float4x4 view;
    float4x4 proj;
    float3 ambientColor;
    float3 cameraPos;
    int4 lightCountPad;         // x=lightCount
    LightData lights[MAX_LIGHTS];
    float4x4 lightVP;           // Shadow map light view-projection
    int4 shadowParams;          // x=shadowEnabled
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] Sampler2D shadowMap;

#ifdef USE_BINDLESS
// --- Bindless path (PC) ---
// Set 1 binding 0: Sampler array
[[vk::binding(0, 1)]] SamplerState bindlessSamplers[256];
// Set 1 binding 1: Texture2D array (unbounded)
[[vk::binding(1, 1)]] Texture2D bindlessTextures[];
// Set 1 binding 2: Material entries SSBO
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

// Push constant: per-object (with materialIndex)
struct PushConstants
{
    float4x4 model;
    int highlighted;
    uint materialIndex;
};

#else
// --- Non-bindless path (current) ---
// Set 1: per-material (shader-specific)
struct LitMaterialParams
{
    float4 baseColor;
    float metallic;
    float roughness;
};

[[vk::binding(0, 1)]] ConstantBuffer<LitMaterialParams> materialParams;
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

static const float PI = 3.14159265359;

float distributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(float NdotV, float NdotL, float roughness)
{
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

float3 fresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// PCF shadow sampling (3x3 kernel)
float sampleShadow(float3 worldPos)
{
    if (frame.shadowParams.x == 0) return 1.0;

    float4 lightClip = mul(frame.lightVP, float4(worldPos, 1.0));
    float3 lightNDC = lightClip.xyz / lightClip.w;
    // NDC [-1,1] → UV [0,1]
    float2 shadowUV = lightNDC.xy * 0.5 + 0.5;
    // D3D12 viewport Y 与 Vulkan 相反，需要翻转 shadow UV Y
    if (frame.shadowParams.y != 0) shadowUV.y = 1.0 - shadowUV.y;
    float currentDepth = lightNDC.z;
    // GLES: lightVP 有 depthFix 使 NDC z 在 [-1,1]，shadow map 存 [0,1]
    // 需要转换 currentDepth 到 [0,1] 匹配 shadow map
    if (frame.shadowParams.z != 0)
        currentDepth = currentDepth * 0.5 + 0.5;

    // Out of shadow map bounds → no shadow
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0)
        return 1.0;

    // PCF 3x3
    float shadow = 0.0;
    float2 texelSize = float2(1.0 / 2048.0, 1.0 / 2048.0);
    float bias = 0.002;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            float closestDepth = shadowMap.Sample(shadowUV + float2(x, y) * texelSize).r;
            shadow += (currentDepth - bias > closestDepth) ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

// 计算单个光源对片元的贡献
float3 evaluateLight(LightData light, float3 N, float3 V, float3 worldPos,
                     float3 albedo, float metallic, float roughness, float3 F0,
                     float shadow = 1.0)
{
    int lightType = int(light.positionAndType.w);
    float3 lightColor = light.colorAndIntensity.xyz;
    float3 L;
    float attenuation = 1.0;

    if (lightType == LIGHT_DIRECTIONAL) {
        L = normalize(-light.directionAndRange.xyz);
    } else {
        // Point / Spot: 从光源位置到片元
        float3 toLight = light.positionAndType.xyz - worldPos;
        float dist = length(toLight);
        L = toLight / max(dist, 0.0001);
        // 距离衰减 (inverse square with range falloff)
        float range = light.directionAndRange.w;
        float distRatio = saturate(1.0 - pow(dist / range, 4.0));
        attenuation = distRatio * distRatio / (dist * dist + 1.0);

        if (lightType == LIGHT_SPOT) {
            // 聚光灯锥形衰减
            float cosAngle = dot(-L, normalize(light.directionAndRange.xyz));
            float cosInner = light.spotParams.x;
            float cosOuter = light.spotParams.y;
            attenuation *= saturate((cosAngle - cosOuter) / max(cosInner - cosOuter, 0.0001));
        }
    }

    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    float3 F = fresnelSchlick(HdotV, F0);

    float3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    float3 kD = (float3(1.0) - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * lightColor * NdotL * attenuation * shadow;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    // Wireframe highlight pass
    if (input.highlighted > 0.5)
        return input.baseColor;

#ifdef USE_BINDLESS
    MaterialEntry entry = materialEntries[pc.materialIndex];
    float3 normalSample = bindlessTextures[NonUniformResourceIndex(entry.normalTexIndex)]
                          .Sample(bindlessSamplers[NonUniformResourceIndex(entry.samplerIndex)], input.texCoord).rgb;
    float3 albedoSample = bindlessTextures[NonUniformResourceIndex(entry.albedoTexIndex)]
                          .Sample(bindlessSamplers[NonUniformResourceIndex(entry.samplerIndex)], input.texCoord).rgb;
#else
    float3 normalSample = normalMap.Sample(input.texCoord).rgb;
    float3 albedoSample = albedoMap.Sample(input.texCoord).rgb;
#endif

    // Normal mapping via screen-space derivatives
    float3 N = normalize(input.normal);
    float3 mapN = normalSample * 2.0 - 1.0;
    float3 Q1 = ddx(input.worldPos);
    float3 Q2 = ddy(input.worldPos);
    float2 st1 = ddx(input.texCoord);
    float2 st2 = ddy(input.texCoord);
    float3 T = normalize(Q1 * st2.y - Q2 * st1.y);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);
    N = normalize(mul(mapN, TBN));

    float3 albedo = albedoSample * input.baseColor.rgb;
    float metallic = input.metallic;
    float roughness = max(input.roughness, 0.04);

    float3 V = normalize(frame.cameraPos - input.worldPos);
    float3 F0 = lerp(float3(0.04), albedo, metallic);

    // Shadow sampling (applied to first directional light only)
    float shadowFactor = sampleShadow(input.worldPos);

    // 累加所有光源贡献
    float3 Lo = float3(0.0);
    int lightCount = frame.lightCountPad.x;
    bool shadowApplied = false;
    for (int i = 0; i < lightCount && i < MAX_LIGHTS; i++) {
        float s = 1.0;
        // Apply shadow to the first directional light
        if (!shadowApplied && int(frame.lights[i].positionAndType.w) == LIGHT_DIRECTIONAL) {
            s = shadowFactor;
            shadowApplied = true;
        }
        Lo += evaluateLight(frame.lights[i], N, V, input.worldPos,
                           albedo, metallic, roughness, F0, s);
    }

    float3 ambient = frame.ambientColor * albedo;
    float3 result = ambient + Lo;

    return float4(result, input.baseColor.a);
}
