// Infinite grid shader (no vertex input, fullscreen quad)

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
[[vk::binding(1, 0)]] Sampler2D shadowMap;

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

// 4x4 matrix inverse (cofactor method)
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

struct FSOutput
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};

// Metal: Slang 的 Metal target 不支持 fwidth，用 ddx/ddy 替代
float2 myFwidth(float2 v)
{
#if __METAL__
    return abs(ddx(v)) + abs(ddy(v));
#else
    return fwidth(v);
#endif
}

float myFwidthF(float v)
{
#if __METAL__
    return abs(ddx(v)) + abs(ddy(v));
#else
    return fwidth(v);
#endif
}

float4 gridLine(float3 fragPos, float scale, float4 color)
{
    float2 coord = fragPos.xz / scale;
    float2 deriv = myFwidth(coord);
    float2 g = abs(frac(coord - 0.5) - 0.5) / (deriv * 0.5);
    float line = min(g.x, g.y);
    float alpha = 1.0 - min(line, 1.0);
    return float4(color.rgb, alpha * color.a);
}

[shader("fragment")]
FSOutput fragmentMain(VSOutput input)
{
    FSOutput output;

    float t = -input.nearPoint.y / (input.farPoint.y - input.nearPoint.y);

    // Metal target 不支持 discard，用 alpha=0 + early return 替代
#if __METAL__
    if (t < 0.0 || t > 1.0) {
        output.color = float4(0, 0, 0, 0);
        output.depth = 1.0;
        return output;
    }
#else
    if (t < 0.0 || t > 1.0)
        discard;
#endif

    float3 fragPos = input.nearPoint + t * (input.farPoint - input.nearPoint);

    float dist = length(fragPos.xz - frame.cameraPos.xz);
    float fade = 1.0 - smoothstep(20.0, 50.0, dist);

    float4 smallGrid = gridLine(fragPos, 0.5, float4(0.4, 0.4, 0.4, 0.4));
    float4 largeGrid = gridLine(fragPos, 5.0, float4(0.5, 0.5, 0.5, 0.7));

    float xAxis = 1.0 - min(abs(fragPos.z) / myFwidthF(fragPos.z), 1.0);
    float zAxis = 1.0 - min(abs(fragPos.x) / myFwidthF(fragPos.x), 1.0);

    float4 color = smallGrid;
    color = lerp(color, largeGrid, largeGrid.a);

    if (xAxis > 0.5) color = float4(1.0, 0.2, 0.2, 1.0);
    if (zAxis > 0.5) color = float4(0.2, 0.2, 1.0, 1.0);

    color.a *= fade;

#if __METAL__
    if (color.a < 0.01) {
        output.color = float4(0, 0, 0, 0);
        output.depth = 1.0;
        return output;
    }
#else
    if (color.a < 0.01)
        discard;
#endif

    output.color = color;

    float4 clipPos = mul(frame.proj, mul(frame.view, float4(fragPos, 1.0)));
    output.depth = clipPos.z / clipPos.w;
    // GLES: depthFix 矩阵使 NDC z 在 [-1,1]，但 gl_FragDepth 需要 [0,1]
    // shadowParams.z != 0 表示需要手动转换
    if (frame.shadowParams.z != 0) {
        output.depth = output.depth * 0.5 + 0.5;
        // 移动端 GLES 上 grid 与 ground 的深度非常接近，
        // 轻微前推一点深度，保持与 Vulkan 一致的覆盖关系。
        output.depth = max(output.depth - 0.0005, 0.0);
    }
    return output;
}
