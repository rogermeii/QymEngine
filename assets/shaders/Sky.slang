// Procedural skybox rendered from a single equirectangular panorama.

static const int LIGHT_DIRECTIONAL = 0;
static const int MAX_LIGHTS = 8;
static const float PI = 3.14159265;

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
[[vk::binding(0, 1)]] Sampler2D skyPanorama;

struct VSOutput
{
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float3 nearPoint : TEXCOORD0;
    [[vk::location(1)]] float3 farPoint : TEXCOORD1;
};

static const float3 positions[6] = {
    float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0),
    float3(-1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)
};

float4x4 mat4Inverse(float4x4 m)
{
    float n11 = m[0][0], n12 = m[0][1], n13 = m[0][2], n14 = m[0][3];
    float n21 = m[1][0], n22 = m[1][1], n23 = m[1][2], n24 = m[1][3];
    float n31 = m[2][0], n32 = m[2][1], n33 = m[2][2], n34 = m[2][3];
    float n41 = m[3][0], n42 = m[3][1], n43 = m[3][2], n44 = m[3][3];

    float t11 = n23*n34*n42 - n24*n33*n42 + n24*n32*n43 - n22*n34*n43 - n23*n32*n44 + n22*n33*n44;
    float t12 = n14*n33*n42 - n13*n34*n42 - n14*n32*n43 + n12*n34*n43 + n13*n32*n44 - n12*n33*n44;
    float t13 = n13*n24*n42 - n14*n23*n42 + n14*n22*n43 - n12*n24*n43 - n13*n22*n44 + n12*n23*n44;
    float t14 = n14*n23*n32 - n13*n24*n32 - n14*n22*n33 + n12*n24*n33 + n13*n22*n34 - n12*n23*n34;

    float det = n11*t11 + n21*t12 + n31*t13 + n41*t14;
    float idet = 1.0 / det;

    float4x4 r;
    r[0][0] = t11 * idet;
    r[0][1] = t12 * idet;
    r[0][2] = t13 * idet;
    r[0][3] = t14 * idet;
    r[1][0] = (n24*n33*n41 - n23*n34*n41 - n24*n31*n43 + n21*n34*n43 + n23*n31*n44 - n21*n33*n44) * idet;
    r[1][1] = (n13*n34*n41 - n14*n33*n41 + n14*n31*n43 - n11*n34*n43 - n13*n31*n44 + n11*n33*n44) * idet;
    r[1][2] = (n14*n23*n41 - n13*n24*n41 - n14*n21*n43 + n11*n24*n43 + n13*n21*n44 - n11*n23*n44) * idet;
    r[1][3] = (n13*n24*n31 - n14*n23*n31 + n14*n21*n33 - n11*n24*n33 - n13*n21*n34 + n11*n23*n34) * idet;
    r[2][0] = (n22*n34*n41 - n24*n32*n41 + n24*n31*n42 - n21*n34*n42 - n22*n31*n44 + n21*n32*n44) * idet;
    r[2][1] = (n14*n32*n41 - n12*n34*n41 - n14*n31*n42 + n11*n34*n42 + n12*n31*n44 - n11*n32*n44) * idet;
    r[2][2] = (n12*n24*n41 - n14*n22*n41 + n14*n21*n42 - n11*n24*n42 - n12*n21*n44 + n11*n22*n44) * idet;
    r[2][3] = (n14*n22*n31 - n12*n24*n31 - n14*n21*n32 + n11*n24*n32 + n12*n21*n34 - n11*n22*n34) * idet;
    r[3][0] = (n23*n32*n41 - n22*n33*n41 - n23*n31*n42 + n21*n33*n42 + n22*n31*n43 - n21*n32*n43) * idet;
    r[3][1] = (n12*n33*n41 - n13*n32*n41 + n13*n31*n42 - n11*n33*n42 - n12*n31*n43 + n11*n32*n43) * idet;
    r[3][2] = (n13*n22*n41 - n12*n23*n41 - n13*n21*n42 + n11*n23*n42 + n12*n21*n43 - n11*n22*n43) * idet;
    r[3][3] = (n12*n23*n31 - n13*n22*n31 + n13*n21*n32 - n11*n23*n32 - n12*n21*n33 + n11*n22*n33) * idet;
    return r;
}

float3 unprojectPoint(float3 p)
{
    float4x4 viewProjInv = mat4Inverse(mul(frame.proj, frame.view));
    float4 unprojected = mul(viewProjInv, float4(p, 1.0));
    return unprojected.xyz / unprojected.w;
}

float3 getPrimarySunDirection()
{
    int lightCount = frame.lightCountPad.x;
    for (int i = 0; i < lightCount && i < MAX_LIGHTS; i++) {
        if (int(frame.lights[i].positionAndType.w) == LIGHT_DIRECTIONAL) {
            return normalize(-frame.lights[i].directionAndRange.xyz);
        }
    }
    return normalize(float3(0.4, 0.8, 0.2));
}

float2 directionToPanoramaUV(float3 dir)
{
    float3 n = normalize(dir);
    float u = atan2(n.z, n.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(n.y, -1.0, 1.0)) / PI;
    return float2(frac(u), saturate(v));
}

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    float3 p = positions[vertexID];
    output.nearPoint = unprojectPoint(float3(p.xy, 0.0));
    output.farPoint = unprojectPoint(float3(p.xy, 1.0));
    output.sv_position = float4(p, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float3 viewDir = normalize(input.farPoint - input.nearPoint);
    float2 uv = directionToPanoramaUV(viewDir);

    float3 panorama = skyPanorama.Sample(uv).rgb;

    float3 sunDir = getPrimarySunDirection();
    float sunDot = saturate(dot(viewDir, sunDir));
    float sunHalo = pow(sunDot, 64.0);
    float sunDisk = pow(sunDot, 2048.0);

    float horizon = saturate(1.0 - abs(viewDir.y));
    float3 horizonTint = lerp(float3(1.0, 1.0, 1.0), float3(1.08, 0.96, 0.86), horizon * 0.35);
    float3 color = panorama * horizonTint;
    color += float3(1.0, 0.78, 0.52) * sunHalo * 0.18;
    color += float3(1.0, 0.95, 0.86) * sunDisk * 4.0;

    return float4(color, 1.0);
}
