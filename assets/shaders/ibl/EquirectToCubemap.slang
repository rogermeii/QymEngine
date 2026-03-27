// 全景图 (equirectangular) → cubemap face 转换着色器

[[vk::binding(0, 0)]] Sampler2D panorama;

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
        case 0: dir = float3( 1.0, -st.y, -st.x); break; // +X
        case 1: dir = float3(-1.0, -st.y,  st.x); break; // -X
        case 2: dir = float3( st.x,  1.0,  st.y); break; // +Y
        case 3: dir = float3( st.x, -1.0, -st.y); break; // -Y
        case 4: dir = float3( st.x, -st.y,  1.0); break; // +Z
        default: dir = float3(-st.x, -st.y, -1.0); break; // -Z
    }
    return normalize(dir);
}

float2 directionToPanoramaUV(float3 dir)
{
    float3 n = normalize(dir);
    float u = atan2(n.z, n.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(n.y, -1.0, 1.0)) / PI;
    return float2(frac(u), saturate(v));
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float3 dir = uvToDirection(input.uv, pc.faceIndex);
    float2 panoUV = directionToPanoramaUV(dir);
    float3 color = panorama.Sample(panoUV).rgb;
    return float4(color, 1.0);
}
