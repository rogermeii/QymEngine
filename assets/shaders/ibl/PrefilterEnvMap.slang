// 镜面反射 GGX 预过滤环境贴图着色器
// 使用 importance sampling 对 cubemap 进行 GGX 卷积

[[vk::binding(0, 0)]] SamplerCube envMap;

struct PushConstants
{
    int faceIndex;
    float roughness;
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
    return o;
}

static const float PI = 3.14159265359;
static const uint SAMPLE_COUNT = 128u;

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

// Van der Corput 低差异序列
float radicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), radicalInverse_VdC(i));
}

// GGX importance sampling
float3 importanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    // 球坐标 → 笛卡尔坐标
    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    // 切线空间 → 世界空间
    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    float3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float3 N = uvToDirection(input.uv, pc.faceIndex);
    // 简化假设：视线方向 = 法线方向 = 反射方向
    float3 R = N;
    float3 V = R;

    float roughness = pc.roughness;
    float totalWeight = 0.0;
    float3 prefilteredColor = float3(0.0);

    for (uint i = 0u; i < SAMPLE_COUNT; i++) {
        float2 Xi = hammersley(i, SAMPLE_COUNT);
        float3 H = importanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            prefilteredColor += envMap.Sample(L).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = prefilteredColor / max(totalWeight, 0.001);
    return float4(prefilteredColor, 1.0);
}
