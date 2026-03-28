// Shadow depth-only shader
// Uses frame.view/proj as light VP matrix (set by shadow pass)

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
[[vk::binding(1, 0)]] Sampler2D shadowMap; // unused but must match Set 0 layout
[[vk::binding(2, 0)]] SamplerCube irradianceMap; // unused but must match Set 0 layout
[[vk::binding(3, 0)]] SamplerCube prefilteredMap; // unused but must match Set 0 layout
[[vk::binding(4, 0)]] Sampler2D brdfLUT; // unused but must match Set 0 layout

struct PushConstants
{
    float4x4 model;
    int highlighted;
};

[[vk::push_constant]] PushConstants pc;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
};

[shader("vertex")]
float4 vertexMain(VSInput input) : SV_Position
{
    float4 worldPos = mul(pc.model, float4(input.position, 1.0));
    return mul(frame.lightVP, worldPos);
}

[shader("fragment")]
void fragmentMain() {}
