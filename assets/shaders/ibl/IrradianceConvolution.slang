// 漫反射 irradiance 卷积着色器
// 对 cubemap 做半球 cosine-weighted 积分

[[vk::binding(0, 0)]] SamplerCube envMap;

struct PushConstants
{
    int faceIndex;
    float _pad;
};

[[vk::push_constant]] PushConstants pc;

struct VSOutput
{
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

static const float3 positions[3] = {
    float3(-1, -1, 0), float3(3, -1, 0), float3(-1, 3, 0)
};

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID)
{
    VSOutput o;
    float3 p = positions[vertexID];
    o.sv_position = float4(p, 1.0);
    o.uv = p.xy * 0.5 + 0.5;
#ifdef GRAPHICS_FLIP_Y
    o.uv.y = 1.0 - o.uv.y;
#endif
    return o;
}

static const float PI = 3.14159265359;

float3 uvToDirection(float2 uv, int face)
{
    float2 st = uv * 2.0 - 1.0;
    float3 dir;
    switch (face) {
        case 0: dir = float3( 1.0, -st.y, -st.x); break;
        case 1: dir = float3(-1.0, -st.y,  st.x); break;
        case 2: dir = float3( st.x,  1.0,  st.y); break;
        case 3: dir = float3( st.x, -1.0, -st.y); break;
        case 4: dir = float3( st.x, -st.y,  1.0); break;
        default: dir = float3(-st.x, -st.y, -1.0); break;
    }
    return normalize(dir);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float3 N = uvToDirection(input.uv, pc.faceIndex);

    // 构建切线空间
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, N));
    up = cross(N, right);

    float3 irradiance = float3(0.0);
    float sampleCount = 0.0;

    // 均匀分布的半球采样 (cosine-weighted)
    static const int PHI_SAMPLES = 8;
    static const int THETA_SAMPLES = 8;
    float deltaPhi = 2.0 * PI / float(PHI_SAMPLES);
    float deltaTheta = 0.5 * PI / float(THETA_SAMPLES);

    for (int p = 0; p < PHI_SAMPLES; p++) {
        for (int t = 0; t < THETA_SAMPLES; t++) {
            float phi = float(p) * deltaPhi;
            float theta = float(t) * deltaTheta;

            // 球坐标 → 切线空间 → 世界空间
            float3 tangentSample = float3(
                sin(theta) * cos(phi),
                sin(theta) * sin(phi),
                cos(theta)
            );
            float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

            irradiance += envMap.Sample(sampleVec).rgb * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }

    irradiance = PI * irradiance / sampleCount;
    return float4(irradiance, 1.0);
}
