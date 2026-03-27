// BRDF 积分 LUT 生成着色器
// 输入: UV (x=NdotV, y=roughness)
// 输出: R=scale, G=bias (Split-Sum 第二项)

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
static const uint SAMPLE_COUNT = 1024u;

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

float3 importanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float geometrySchlickGGX(float NdotV, float roughness)
{
    // IBL 用的 k 不同于直接光照
    float a = roughness;
    float k = (a * a) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(float NdotV, float NdotL, float roughness)
{
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float NdotV = input.uv.x;
    float roughness = input.uv.y;

    // 避免 NdotV = 0 导致除零
    NdotV = max(NdotV, 0.001);

    float3 V;
    V.x = sqrt(1.0 - NdotV * NdotV); // sinTheta
    V.y = 0.0;
    V.z = NdotV; // cosTheta

    float3 N = float3(0.0, 0.0, 1.0);

    float A = 0.0; // scale
    float B = 0.0; // bias

    for (uint i = 0u; i < SAMPLE_COUNT; i++) {
        float2 Xi = hammersley(i, SAMPLE_COUNT);
        float3 H = importanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0) {
            float G = geometrySmith(NdotV, NdotL, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);

    return float4(A, B, 0.0, 1.0);
}
