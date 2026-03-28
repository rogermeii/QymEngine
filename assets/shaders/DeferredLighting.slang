// 延迟渲染 Lighting Pass: 从 G-Buffer 读取材质信息，执行 PBR 光照

static const int LIGHT_DIRECTIONAL = 0;
static const int LIGHT_POINT       = 1;
static const int LIGHT_SPOT        = 2;
static const int MAX_LIGHTS        = 8;
static const float PI = 3.14159265359;

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

// Set 0: G-Buffer textures + 场景数据
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] Sampler2D gbufferRT0;      // Albedo.rgb + Metallic
[[vk::binding(2, 0)]] Sampler2D gbufferRT1;      // Normal.xy (八面体编码)
[[vk::binding(3, 0)]] Sampler2D gbufferRT2;      // Roughness + AO
[[vk::binding(4, 0)]] Sampler2D depthTex;        // 深度缓冲
[[vk::binding(5, 0)]] Sampler2D shadowMap;
[[vk::binding(6, 0)]] SamplerCube irradianceMap;
[[vk::binding(7, 0)]] SamplerCube prefilteredMap;
[[vk::binding(8, 0)]] Sampler2D brdfLUT;
[[vk::binding(9, 0)]] Sampler2D ssaoTex;         // SSAO 输出（可选）

struct LightingPushConstant {
    float2 screenSize;       // 屏幕分辨率
    int ssaoEnabled;
    int _pad;
};

[[vk::push_constant]] LightingPushConstant pc;

struct VSOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

static const float3 positions[6] = {
    float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0),
    float3(-1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)
};

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID) {
    VSOutput o;
    float3 p = positions[vertexID];
    o.sv_position = float4(p, 1.0);
    o.uv = p.xy * 0.5 + 0.5;
#ifdef GRAPHICS_FLIP_Y
    o.uv.y = 1.0 - o.uv.y;
#endif
    return o;
}

// 八面体法线解码
float3 octDecode(float2 e)
{
    e = e * 2.0 - 1.0; // [0,1] → [-1,1]
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// 从深度缓冲重建世界坐标
float3 reconstructWorldPos(float2 uv, float depth)
{
    // NDC 坐标
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    // 逆 VP 变换
    float4x4 invView = mat4Inverse(frame.view);
    float4x4 invProj = mat4Inverse(frame.proj);
    float4 viewPos = mul(invProj, clipPos);
    viewPos /= viewPos.w;
    float4 worldPos = mul(invView, viewPos);
    return worldPos.xyz;
}

// 4x4 矩阵求逆（Slang 内置辅助）
float4x4 mat4Inverse(float4x4 m)
{
    float n11 = m[0][0], n12 = m[1][0], n13 = m[2][0], n14 = m[3][0];
    float n21 = m[0][1], n22 = m[1][1], n23 = m[2][1], n24 = m[3][1];
    float n31 = m[0][2], n32 = m[1][2], n33 = m[2][2], n34 = m[3][2];
    float n41 = m[0][3], n42 = m[1][3], n43 = m[2][3], n44 = m[3][3];

    float t11 = n23*n34*n42 - n24*n33*n42 + n24*n32*n43 - n22*n34*n43 - n23*n32*n44 + n22*n33*n44;
    float t12 = n14*n33*n42 - n13*n34*n42 - n14*n32*n43 + n12*n34*n43 + n13*n32*n44 - n12*n33*n44;
    float t13 = n13*n24*n42 - n14*n23*n42 + n14*n22*n43 - n12*n24*n43 - n13*n22*n44 + n12*n23*n44;
    float t14 = n14*n23*n32 - n13*n24*n32 - n14*n22*n33 + n12*n24*n33 + n13*n22*n34 - n12*n23*n34;

    float det = n11*t11 + n21*t12 + n31*t13 + n41*t14;
    float idet = 1.0 / det;

    float4x4 ret;
    ret[0][0] = t11 * idet;
    ret[0][1] = (n24*n33*n41 - n23*n34*n41 - n24*n31*n43 + n21*n34*n43 + n23*n31*n44 - n21*n33*n44) * idet;
    ret[0][2] = (n22*n34*n41 - n24*n32*n41 + n24*n31*n42 - n21*n34*n42 - n22*n31*n44 + n21*n32*n44) * idet;
    ret[0][3] = (n23*n32*n41 - n22*n33*n41 - n23*n31*n42 + n21*n33*n42 + n22*n31*n43 - n21*n32*n43) * idet;
    ret[1][0] = t12 * idet;
    ret[1][1] = (n13*n34*n41 - n14*n33*n41 + n14*n31*n43 - n11*n34*n43 - n13*n31*n44 + n11*n33*n44) * idet;
    ret[1][2] = (n14*n32*n41 - n12*n34*n41 - n14*n31*n42 + n11*n34*n42 + n12*n31*n44 - n11*n32*n44) * idet;
    ret[1][3] = (n12*n33*n41 - n13*n32*n41 + n13*n31*n42 - n11*n33*n42 - n12*n31*n43 + n11*n32*n43) * idet;
    ret[2][0] = t13 * idet;
    ret[2][1] = (n14*n23*n41 - n13*n24*n41 - n14*n21*n43 + n11*n24*n43 + n13*n21*n44 - n11*n23*n44) * idet;
    ret[2][2] = (n12*n24*n41 - n14*n22*n41 + n14*n21*n42 - n11*n24*n42 - n12*n21*n44 + n11*n22*n44) * idet;
    ret[2][3] = (n13*n22*n41 - n12*n23*n41 - n13*n21*n42 + n11*n23*n42 + n12*n21*n43 - n11*n22*n43) * idet;
    ret[3][0] = t14 * idet;
    ret[3][1] = (n13*n24*n31 - n14*n23*n31 + n14*n21*n33 - n11*n24*n33 - n13*n21*n34 + n11*n23*n34) * idet;
    ret[3][2] = (n14*n22*n31 - n12*n24*n31 - n14*n21*n32 + n11*n24*n32 + n12*n21*n34 - n11*n22*n34) * idet;
    ret[3][3] = (n12*n23*n31 - n13*n22*n31 + n13*n21*n32 - n11*n23*n32 - n12*n21*n33 + n11*n22*n33) * idet;
    return ret;
}

// --- PBR 光照函数（从 Lit.slang 提取） ---

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

float sampleShadow(float3 worldPos)
{
    if (frame.shadowParams.x == 0) return 1.0;

    float4 lightClip = mul(frame.lightVP, float4(worldPos, 1.0));
    float3 lightNDC = lightClip.xyz / lightClip.w;
    float2 shadowUV = lightNDC.xy * 0.5 + 0.5;
    if (frame.shadowParams.y != 0) shadowUV.y = 1.0 - shadowUV.y;
    float currentDepth = lightNDC.z;
    if (frame.shadowParams.z != 0)
        currentDepth = currentDepth * 0.5 + 0.5;

    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0)
        return 1.0;

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

float3 evaluateLight(LightData light, float3 N, float3 V, float3 worldPos,
                     float3 albedo, float metallic, float roughness, float3 F0,
                     float shadow)
{
    int lightType = int(light.positionAndType.w);
    float3 lightColor = light.colorAndIntensity.xyz;
    float3 L;
    float attenuation = 1.0;

    if (lightType == LIGHT_DIRECTIONAL) {
        L = normalize(-light.directionAndRange.xyz);
    } else {
        float3 toLight = light.positionAndType.xyz - worldPos;
        float dist = length(toLight);
        L = toLight / max(dist, 0.0001);
        float range = light.directionAndRange.w;
        float distRatio = saturate(1.0 - pow(dist / range, 4.0));
        attenuation = distRatio * distRatio / (dist * dist + 1.0);

        if (lightType == LIGHT_SPOT) {
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

// --- Fragment shader ---

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float2 uv = input.uv;

    // 读取 G-Buffer
    float4 rt0 = gbufferRT0.Sample(uv);
    float2 rt1 = gbufferRT1.Sample(uv).rg;
    float4 rt2 = gbufferRT2.Sample(uv);
    float depth = depthTex.Sample(uv).r;

    // 天空像素（深度 = 1.0）直接跳过
    if (depth >= 0.9999) discard;

    float3 albedo = rt0.rgb;
    float metallic = rt0.a;
    float3 N = octDecode(rt1);
    float roughness = max(rt2.r, 0.04);
    float ao = rt2.g;

    // SSAO
    if (pc.ssaoEnabled != 0) {
        float ssaoValue = ssaoTex.Sample(uv).r;
        ao *= ssaoValue;
    }

    // 世界坐标重建
    float3 worldPos = reconstructWorldPos(uv, depth);
    float3 V = normalize(frame.cameraPos - worldPos);
    float3 F0 = lerp(float3(0.04), albedo, metallic);

    // 阴影
    float shadowFactor = sampleShadow(worldPos);

    // 累加光源
    float3 Lo = float3(0.0);
    int lightCount = frame.lightCountPad.x;
    bool shadowApplied = false;
    for (int i = 0; i < lightCount && i < MAX_LIGHTS; i++) {
        float s = 1.0;
        if (!shadowApplied && int(frame.lights[i].positionAndType.w) == LIGHT_DIRECTIONAL) {
            s = shadowFactor;
            shadowApplied = true;
        }
        Lo += evaluateLight(frame.lights[i], N, V, worldPos, albedo, metallic, roughness, F0, s);
    }

    // IBL
    float3 R = reflect(-V, N);
    float NdotV = max(dot(N, V), 0.0);

    float3 irradiance = irradianceMap.Sample(N).rgb;
    float3 kS = fresnelSchlick(NdotV, F0);
    float3 kD = (float3(1.0) - kS) * (1.0 - metallic);
    float3 diffuseIBL = irradiance * albedo * kD;

    float3 prefilteredColor = prefilteredMap.SampleLevel(R, roughness * 4.0).rgb;
    float2 brdf = brdfLUT.Sample(float2(NdotV, roughness)).rg;
    float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

    float3 ambient = (diffuseIBL + specularIBL) * frame.ambientColor * ao;
    float3 result = ambient + Lo;

    return float4(result, 1.0);
}
