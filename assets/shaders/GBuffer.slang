// G-Buffer 几何 pass: 输出材质信息到 MRT（延迟渲染用）
// RT0: Albedo.rgb + Metallic (RGBA8)
// RT1: Normal.xy 八面体编码 (RG16F)
// RT2: Roughness + AO + 保留 (RGBA8)

static const int MAX_LIGHTS = 8;

struct LightData
{
    float4 positionAndType;
    float4 directionAndRange;
    float4 colorAndIntensity;
    float4 spotParams;
};

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
[[vk::binding(1, 0)]] Sampler2D shadowMap;       // 占位，匹配 Set 0 布局
[[vk::binding(2, 0)]] SamplerCube irradianceMap;  // 占位
[[vk::binding(3, 0)]] SamplerCube prefilteredMap; // 占位
[[vk::binding(4, 0)]] Sampler2D brdfLUT;          // 占位

// Set 1: 材质参数
struct LitMaterialParams
{
    float4 baseColor;
    float metallic;
    float roughness;
};

[[vk::binding(0, 1)]] ConstantBuffer<LitMaterialParams> materialParams;
[[vk::binding(1, 1)]] Sampler2D albedoMap;
[[vk::binding(2, 1)]] Sampler2D normalMap; // 占位，匹配 Lit shader 的 Set 1 布局

struct PushConstants
{
    float4x4 model;
    int highlighted;
};

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
    [[vk::location(0)]] float2 texCoord : TEXCOORD0;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float metallic : TEXCOORD1;
    [[vk::location(3)]] float roughness : TEXCOORD2;
    [[vk::location(4)]] float4 baseColor : TEXCOORD3;
};

// 八面体法线编码：将单位法线编码为 2 个 float [-1, 1]
float2 octEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        float2 wrapped = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = wrapped;
    }
    return n.xy * 0.5 + 0.5; // 映射到 [0, 1] 方便存 unorm/float16
}

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(pc.model, float4(input.position, 1.0));
    output.sv_position = mul(frame.proj, mul(frame.view, worldPos));
    output.texCoord = input.texCoord;

    float3x3 modelMat3 = (float3x3)pc.model;
    output.normal = normalize(mul(modelMat3, input.normal));

    output.metallic = materialParams.metallic;
    output.roughness = materialParams.roughness;
    output.baseColor = materialParams.baseColor;

    return output;
}

struct GBufferOutput
{
    [[vk::location(0)]] float4 rt0 : SV_Target0; // Albedo.rgb + Metallic
    [[vk::location(1)]] float2 rt1 : SV_Target1; // Normal.xy (八面体编码)
    [[vk::location(2)]] float4 rt2 : SV_Target2; // Roughness + AO + 保留
};

[shader("fragment")]
GBufferOutput fragmentMain(VSOutput input)
{
    GBufferOutput output;

    float4 texColor = albedoMap.Sample(input.texCoord);
    float3 normalSample = normalMap.Sample(input.texCoord).rgb;
    // 引用所有 Set 1 bindings 确保 Slang 保留 FS stage flag（与 Lit shader layout 兼容）
    float3 albedo = texColor.rgb * input.baseColor.rgb;

    // 法线贴图应用（简化版，无 TBN）
    float3 N = normalize(input.normal);
    if (length(normalSample - 0.5) > 0.01) {
        // 有法线贴图时微调法线
        N = normalize(N + (normalSample * 2.0 - 1.0) * 0.3);
    }

    output.rt0 = float4(albedo, input.metallic);
    output.rt1 = octEncode(N);
    output.rt2 = float4(input.roughness, 1.0, 0.0, 0.0); // AO = 1.0 默认

    return output;
}
