// Lit PBR shader (Cook-Torrance GGX)

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
    // Normal matrix = transpose(inverse(model))
    // Approximate normal transform: use upper-left 3x3 of model matrix
    // (correct for uniform scale; for non-uniform scale, should use inverse transpose)
    float3x3 modelMat3 = (float3x3)pc.model;
    output.normal = mul(modelMat3, input.normal);
    output.worldPos = worldPos.xyz;
    output.baseColor = materialParams.baseColor;
    output.metallic = materialParams.metallic;
    output.roughness = materialParams.roughness;
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

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    // Wireframe highlight pass
    if (input.highlighted == 1)
        return input.baseColor;

    // Normal mapping via screen-space derivatives
    float3 N = normalize(input.normal);
    float3 mapN = normalMap.Sample(input.texCoord).rgb * 2.0 - 1.0;
    float3 Q1 = ddx(input.worldPos);
    float3 Q2 = ddy(input.worldPos);
    float2 st1 = ddx(input.texCoord);
    float2 st2 = ddy(input.texCoord);
    float3 T = normalize(Q1 * st2.y - Q2 * st1.y);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);
    N = normalize(mul(mapN, TBN));

    float3 albedo = albedoMap.Sample(input.texCoord).rgb * input.baseColor.rgb;
    float metallic = input.metallic;
    float roughness = max(input.roughness, 0.04);

    float3 V = normalize(frame.cameraPos - input.worldPos);
    float3 L = normalize(-frame.lightDir);
    float3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float3 F0 = lerp(float3(0.04), albedo, metallic);

    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    float3 F = fresnelSchlick(HdotV, F0);

    float3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    float3 specular = numerator / denominator;

    float3 kD = (float3(1.0) - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    float3 Lo = (diffuse + specular) * frame.lightColor * NdotL;
    float3 ambient = frame.ambientColor * albedo;
    float3 result = ambient + Lo;

    return float4(result, input.baseColor.a);
}
