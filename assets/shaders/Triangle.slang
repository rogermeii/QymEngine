// Triangle/Basic shader (Blinn-Phong lighting)

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

[[vk::binding(0, 1)]] Sampler2D texSampler;

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
    output.color = input.color;
    output.texCoord = input.texCoord;
    output.highlighted = pc.highlighted;
    float3x3 modelMat3 = (float3x3)pc.model;
    output.normal = mul(modelMat3, input.normal);
    output.worldPos = worldPos.xyz;
    output.baseColor = pc.baseColor;
    output.metallic = pc.metallic;
    output.roughness = pc.roughness;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    if (input.highlighted == 1)
        return input.baseColor;

    float3 texColor = texSampler.Sample(input.texCoord).rgb;
    float3 albedo = texColor * input.baseColor.rgb;
    float3 normal = normalize(input.normal);
    float3 lightD = normalize(-ubo.lightDir);

    float3 ambient = ubo.ambientColor * albedo;

    float diff = max(dot(normal, lightD), 0.0);
    float3 diffuseColor = diff * ubo.lightColor * albedo * (1.0 - input.metallic);

    float3 viewDir = normalize(ubo.cameraPos - input.worldPos);
    float3 halfDir = normalize(lightD + viewDir);
    float shininess = lerp(256.0, 8.0, input.roughness);
    float spec = pow(max(dot(normal, halfDir), 0.0), shininess);
    float specStrength = lerp(0.3, 1.0, input.metallic);
    float3 specular = spec * ubo.lightColor * specStrength;

    float3 result = ambient + diffuseColor + specular;
    return float4(result, input.baseColor.a);
}
