// Infinite grid shader (no vertex input, fullscreen quad)

struct FrameData
{
    float4x4 view;
    float4x4 proj;
    float3 lightDir;
    float3 lightColor;
    float3 ambientColor;
    float3 cameraPos;
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;

struct VSOutput
{
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float3 nearPoint;
    [[vk::location(1)]] float3 farPoint;
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

float4 gridLine(float3 fragPos, float scale, float4 color)
{
    float2 coord = fragPos.xz / scale;
    float2 deriv = fwidth(coord);
    float2 g = abs(frac(coord - 0.5) - 0.5) / deriv;
    float line = min(g.x, g.y);
    float alpha = 1.0 - min(line, 1.0);
    return float4(color.rgb, alpha * color.a);
}

[shader("fragment")]
FSOutput fragmentMain(VSOutput input)
{
    FSOutput output;

    float t = -input.nearPoint.y / (input.farPoint.y - input.nearPoint.y);

    if (t < 0.0 || t > 1.0)
    {
        float3 dir = normalize(input.farPoint - input.nearPoint);
        float h = max(dir.y, 0.0);
        float3 skyBottom = float3(0.4, 0.4, 0.45);
        float3 skyTop = float3(0.15, 0.15, 0.3);
        output.color = float4(lerp(skyBottom, skyTop, h), 1.0);
        output.depth = 1.0;
        return output;
    }

    float3 fragPos = input.nearPoint + t * (input.farPoint - input.nearPoint);

    float dist = length(fragPos.xz - frame.cameraPos.xz);
    float fade = 1.0 - smoothstep(20.0, 50.0, dist);

    float4 smallGrid = gridLine(fragPos, 0.5, float4(0.4, 0.4, 0.4, 0.4));
    float4 largeGrid = gridLine(fragPos, 5.0, float4(0.5, 0.5, 0.5, 0.7));

    float xAxis = 1.0 - min(abs(fragPos.z) / fwidth(fragPos.z), 1.0);
    float zAxis = 1.0 - min(abs(fragPos.x) / fwidth(fragPos.x), 1.0);

    float4 color = smallGrid;
    color = lerp(color, largeGrid, largeGrid.a);

    if (xAxis > 0.5) color = float4(1.0, 0.2, 0.2, 1.0);
    if (zAxis > 0.5) color = float4(0.2, 0.2, 1.0, 1.0);

    color.a *= fade;

    if (color.a < 0.01)
        discard;

    output.color = color;

    float4 clipPos = mul(frame.proj, mul(frame.view, float4(fragPos, 1.0)));
    output.depth = clipPos.z / clipPos.w;
    return output;
}
