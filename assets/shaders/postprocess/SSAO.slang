// SSAO (Screen Space Ambient Occlusion)
// 支持三种算法：经典 SSAO / HBAO / GTAO，通过 push constant 切换

static const int SSAO_CLASSIC = 0;
static const int SSAO_HBAO    = 1;
static const int SSAO_GTAO    = 2;
static const float PI = 3.14159265359;

[[vk::binding(0, 0)]] Sampler2D depthTex;
[[vk::binding(1, 0)]] Sampler2D normalTex;    // G-Buffer RT1（八面体编码法线）
[[vk::binding(2, 0)]] Sampler2D noiseTex;     // 4x4 随机旋转纹理

struct FrameData
{
    float4x4 view;
    float4x4 proj;
};

[[vk::binding(3, 0)]] ConstantBuffer<FrameData> frameSSAO;

struct SSAOPushConstant {
    float2 screenSize;
    float2 noiseScale;        // screenSize / 4.0
    float radius;             // 采样半径（视空间）
    float bias;               // 深度偏移
    float intensity;          // AO 强度
    int algorithm;            // 0=Classic, 1=HBAO, 2=GTAO
    int kernelSize;           // 采样数量（Classic: 32-64, HBAO: directions*steps）
    float power;              // AO 指数
};

[[vk::push_constant]] SSAOPushConstant pc;

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
    e = e * 2.0 - 1.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// 从深度 + UV 重建视空间坐标
float3 getViewPos(float2 uv)
{
    float depth = depthTex.Sample(uv).r;
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4x4 invProj = mat4Inverse(frameSSAO.proj);
    float4 viewPos = mul(invProj, clipPos);
    return viewPos.xyz / viewPos.w;
}

// 从世界法线转视空间法线
float3 getViewNormal(float2 uv)
{
    float2 encodedNormal = normalTex.Sample(uv).rg;
    float3 worldNormal = octDecode(encodedNormal);
    return normalize(mul(float3x3(frameSSAO.view), worldNormal));
}

// 伪随机哈希
float hash(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

// --- 矩阵求逆 ---
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
    ret[0][0] = t11*idet; ret[1][0] = t12*idet; ret[2][0] = t13*idet; ret[3][0] = t14*idet;
    ret[0][1] = (n24*n33*n41-n23*n34*n41-n24*n31*n43+n21*n34*n43+n23*n31*n44-n21*n33*n44)*idet;
    ret[1][1] = (n13*n34*n41-n14*n33*n41+n14*n31*n43-n11*n34*n43-n13*n31*n44+n11*n33*n44)*idet;
    ret[2][1] = (n14*n32*n41-n12*n34*n41-n14*n31*n42+n11*n34*n42+n12*n31*n44-n11*n32*n44)*idet;
    ret[3][1] = (n12*n33*n41-n13*n32*n41+n13*n31*n42-n11*n33*n42-n12*n31*n43+n11*n32*n43)*idet;
    ret[0][2] = (n22*n34*n41-n24*n32*n41+n24*n31*n42-n21*n34*n42-n22*n31*n44+n21*n32*n44)*idet;
    ret[1][2] = (n14*n32*n41-n12*n34*n41-n14*n31*n42+n11*n34*n42+n12*n31*n44-n11*n32*n44)*idet;
    ret[2][2] = (n12*n24*n41-n14*n22*n41+n14*n21*n42-n11*n24*n42-n12*n21*n44+n11*n22*n44)*idet;
    ret[3][2] = (n13*n22*n41-n12*n23*n41-n13*n21*n42+n11*n23*n42+n12*n21*n43-n11*n22*n43)*idet;
    ret[0][3] = (n23*n32*n41-n22*n33*n41-n23*n31*n42+n21*n33*n42+n22*n31*n43-n21*n32*n43)*idet;
    ret[1][3] = (n12*n33*n41-n13*n32*n41+n13*n31*n42-n11*n33*n42-n12*n31*n43+n11*n32*n43)*idet;
    ret[2][3] = (n13*n22*n41-n12*n23*n41-n13*n21*n42+n11*n23*n42+n12*n21*n43-n11*n22*n43)*idet;
    ret[3][3] = (n12*n23*n31-n13*n22*n31+n13*n21*n32-n11*n23*n32-n12*n21*n33+n11*n22*n33)*idet;
    return ret;
}

// =====================================================
// 算法 1: 经典 SSAO (Crytek 2007)
// =====================================================
float classicSSAO(float2 uv)
{
    float3 fragPos = getViewPos(uv);
    float3 normal = getViewNormal(uv);

    // 从噪声纹理获取随机旋转向量
    float3 randomVec = float3(noiseTex.Sample(uv * pc.noiseScale).xy * 2.0 - 1.0, 0.0);

    // 构建 TBN（以法线为 Z 轴）
    float3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 TBN = float3x3(tangent, bitangent, normal);

    float occlusion = 0.0;
    int samples = min(pc.kernelSize, 64);

    for (int i = 0; i < samples; i++) {
        // 半球采样点（预计算在 CPU 侧更好，这里用伪随机）
        float fi = float(i);
        float xi1 = hash(float2(fi * 0.1, uv.x));
        float xi2 = hash(float2(fi * 0.2, uv.y));
        float xi3 = hash(float2(fi * 0.3, uv.x + uv.y));

        float3 sampleVec = float3(
            xi1 * 2.0 - 1.0,
            xi2 * 2.0 - 1.0,
            xi3  // z 在 [0, 1] 保持在半球
        );
        sampleVec = normalize(sampleVec);
        // 按距离缩放（靠近原点的采样点更多）
        float scale = fi / float(samples);
        scale = lerp(0.1, 1.0, scale * scale);
        sampleVec *= scale * pc.radius;

        // 变换到视空间
        float3 samplePos = fragPos + mul(TBN, sampleVec);

        // 投影到屏幕空间
        float4 offset = mul(frameSSAO.proj, float4(samplePos, 1.0));
        offset.xy = offset.xy / offset.w * 0.5 + 0.5;

        // 采样该点的实际深度
        float sampleDepth = getViewPos(offset.xy).z;

        // 范围检查 + 遮蔽判定
        float rangeCheck = smoothstep(0.0, 1.0, pc.radius / abs(fragPos.z - sampleDepth));
        occlusion += (sampleDepth >= samplePos.z + pc.bias ? 1.0 : 0.0) * rangeCheck;
    }

    return 1.0 - (occlusion / float(samples)) * pc.intensity;
}

// =====================================================
// 算法 2: HBAO (Horizon-Based Ambient Occlusion)
// =====================================================
float hbaoSSAO(float2 uv)
{
    float3 fragPos = getViewPos(uv);
    float3 normal = getViewNormal(uv);

    float2 noise = noiseTex.Sample(uv * pc.noiseScale).xy * 2.0 - 1.0;
    float noiseAngle = atan2(noise.y, noise.x);

    int numDirections = 8;
    int numSteps = max(pc.kernelSize / numDirections, 4);
    float occlusion = 0.0;

    for (int d = 0; d < numDirections; d++) {
        float angle = (float(d) / float(numDirections)) * PI * 2.0 + noiseAngle;
        float2 dir = float2(cos(angle), sin(angle));

        // 在该方向上步进
        float horizonAngle = -1.0; // 地平线角度初始化为最低

        for (int s = 1; s <= numSteps; s++) {
            float2 stepUV = uv + dir * (float(s) / float(numSteps)) * pc.radius / fragPos.z * 0.5;
            if (stepUV.x < 0.0 || stepUV.x > 1.0 || stepUV.y < 0.0 || stepUV.y > 1.0) break;

            float3 samplePos = getViewPos(stepUV);
            float3 diff = samplePos - fragPos;
            float dist = length(diff);

            if (dist < 0.001 || dist > pc.radius) continue;

            // 计算样本相对于法线的仰角
            float sampleAngle = dot(normalize(diff), normal);
            horizonAngle = max(horizonAngle, sampleAngle);
        }

        // 地平线角度越高，遮蔽越强
        float ao = saturate(horizonAngle);
        occlusion += ao;
    }

    return 1.0 - (occlusion / float(numDirections)) * pc.intensity;
}

// =====================================================
// 算法 3: GTAO (Ground Truth Ambient Occlusion)
// =====================================================
float gtaoSSAO(float2 uv)
{
    float3 fragPos = getViewPos(uv);
    float3 normal = getViewNormal(uv);

    float2 noise = noiseTex.Sample(uv * pc.noiseScale).xy * 2.0 - 1.0;
    float noiseAngle = atan2(noise.y, noise.x);

    int numDirections = 8;
    int numSteps = max(pc.kernelSize / numDirections, 4);
    float occlusion = 0.0;

    for (int d = 0; d < numDirections; d++) {
        float angle = (float(d) / float(numDirections)) * PI + noiseAngle;
        float2 dir = float2(cos(angle), sin(angle));

        // 正反两个方向的地平线角度
        float h1 = -1.0, h2 = -1.0;

        for (int s = 1; s <= numSteps; s++) {
            float stepScale = float(s) / float(numSteps) * pc.radius / max(-fragPos.z, 0.1) * 0.5;

            // 正方向
            float2 uv1 = uv + dir * stepScale;
            if (uv1.x >= 0.0 && uv1.x <= 1.0 && uv1.y >= 0.0 && uv1.y <= 1.0) {
                float3 diff1 = getViewPos(uv1) - fragPos;
                float dist1 = length(diff1);
                if (dist1 > 0.001 && dist1 < pc.radius) {
                    h1 = max(h1, dot(normalize(diff1), normal));
                }
            }

            // 反方向
            float2 uv2 = uv - dir * stepScale;
            if (uv2.x >= 0.0 && uv2.x <= 1.0 && uv2.y >= 0.0 && uv2.y <= 1.0) {
                float3 diff2 = getViewPos(uv2) - fragPos;
                float dist2 = length(diff2);
                if (dist2 > 0.001 && dist2 < pc.radius) {
                    h2 = max(h2, dot(normalize(diff2), normal));
                }
            }
        }

        // GTAO 解析积分：基于 cos 加权的地平线遮蔽
        float theta1 = acos(saturate(h1));
        float theta2 = acos(saturate(h2));
        // 可见扇形的余弦积分
        float sliceAO = 0.25 * (-cos(2.0 * theta1) + cos(0.0) + 2.0 * theta1 * sin(0.0))
                      + 0.25 * (-cos(2.0 * theta2) + cos(0.0) + 2.0 * theta2 * sin(0.0));
        occlusion += saturate(sliceAO);
    }

    float ao = occlusion / float(numDirections);
    return pow(saturate(ao), pc.power) * pc.intensity + (1.0 - pc.intensity);
}

// =====================================================
// Fragment: 根据 push constant 选择算法
// =====================================================
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float depth = depthTex.Sample(input.uv).r;
    if (depth >= 0.9999) return float4(1.0); // 天空无 AO

    float ao;
    if (pc.algorithm == SSAO_HBAO) {
        ao = hbaoSSAO(input.uv);
    } else if (pc.algorithm == SSAO_GTAO) {
        ao = gtaoSSAO(input.uv);
    } else {
        ao = classicSSAO(input.uv);
    }

    return float4(ao, ao, ao, 1.0);
}
