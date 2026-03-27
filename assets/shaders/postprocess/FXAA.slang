// FXAA 3.11 Quality

[[vk::binding(0, 0)]] Sampler2D inputTex;

struct FxaaPushConstant {
    float2 texelSize;
    float subpixQuality;
    float edgeThreshold;
    float edgeThresholdMin;
};

[[vk::push_constant]] FxaaPushConstant pc;

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

float luminance(float3 c) {
    return dot(c, float3(0.299, 0.587, 0.114));
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 invSize = pc.texelSize;

    float3 rgbM  = inputTex.Sample(uv).rgb;
    float3 rgbN  = inputTex.Sample(uv + float2(0, -invSize.y)).rgb;
    float3 rgbS  = inputTex.Sample(uv + float2(0,  invSize.y)).rgb;
    float3 rgbW  = inputTex.Sample(uv + float2(-invSize.x, 0)).rgb;
    float3 rgbE  = inputTex.Sample(uv + float2( invSize.x, 0)).rgb;

    float lumaM = luminance(rgbM);
    float lumaN = luminance(rgbN);
    float lumaS = luminance(rgbS);
    float lumaW = luminance(rgbW);
    float lumaE = luminance(rgbE);

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(pc.edgeThresholdMin, lumaMax * pc.edgeThreshold)) {
        return float4(rgbM, 1.0);
    }

    float3 rgbNW = inputTex.Sample(uv + float2(-invSize.x, -invSize.y)).rgb;
    float3 rgbNE = inputTex.Sample(uv + float2( invSize.x, -invSize.y)).rgb;
    float3 rgbSW = inputTex.Sample(uv + float2(-invSize.x,  invSize.y)).rgb;
    float3 rgbSE = inputTex.Sample(uv + float2( invSize.x,  invSize.y)).rgb;

    float lumaNW = luminance(rgbNW);
    float lumaNE = luminance(rgbNE);
    float lumaSW = luminance(rgbSW);
    float lumaSE = luminance(rgbSE);

    float lumaAvg = (lumaN + lumaS + lumaW + lumaE) * 0.25;
    float subpixRcpRange = 1.0 / lumaRange;
    float subpixA = saturate(abs(lumaAvg - lumaM) * subpixRcpRange);
    float subpixB = (-2.0 * subpixA + 3.0) * subpixA * subpixA;
    float subpixC = subpixB * subpixB * pc.subpixQuality;

    float edgeH = abs(-2.0 * lumaW + lumaNW + lumaSW) +
                  abs(-2.0 * lumaM + lumaN + lumaS) * 2.0 +
                  abs(-2.0 * lumaE + lumaNE + lumaSE);
    float edgeV = abs(-2.0 * lumaN + lumaNW + lumaNE) +
                  abs(-2.0 * lumaM + lumaW + lumaE) * 2.0 +
                  abs(-2.0 * lumaS + lumaSW + lumaSE);
    bool isHorizontal = (edgeH >= edgeV);

    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float gradient1 = abs(luma1 - lumaM);
    float gradient2 = abs(luma2 - lumaM);
    bool is1Steepest = gradient1 >= gradient2;

    float stepLength = isHorizontal ? invSize.y : invSize.x;
    float gradientScaled = 0.25 * max(gradient1, gradient2);
    float lumaLocalAvg;

    if (is1Steepest) {
        stepLength = -stepLength;
        lumaLocalAvg = 0.5 * (luma1 + lumaM);
    } else {
        lumaLocalAvg = 0.5 * (luma2 + lumaM);
    }

    float2 currentUv = uv;
    if (isHorizontal) {
        currentUv.y += stepLength * 0.5;
    } else {
        currentUv.x += stepLength * 0.5;
    }

    float2 offset = isHorizontal ? float2(invSize.x, 0) : float2(0, invSize.y);

    float2 uv1 = currentUv - offset;
    float2 uv2 = currentUv + offset;

    float lumaEnd1 = luminance(inputTex.Sample(uv1).rgb) - lumaLocalAvg;
    float lumaEnd2 = luminance(inputTex.Sample(uv2).rgb) - lumaLocalAvg;

    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;

    static const float QUALITY[12] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
    for (int i = 0; i < 12 && !reachedBoth; i++) {
        if (!reached1) {
            uv1 -= offset * QUALITY[i];
            lumaEnd1 = luminance(inputTex.Sample(uv1).rgb) - lumaLocalAvg;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2) {
            uv2 += offset * QUALITY[i];
            lumaEnd2 = luminance(inputTex.Sample(uv2).rgb) - lumaLocalAvg;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
        reachedBoth = reached1 && reached2;
    }

    float dist1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float dist2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    float distFinal = min(dist1, dist2);
    float edgeLength = dist1 + dist2;
    float pixelOffset = -distFinal / edgeLength + 0.5;

    bool isLumaMSmaller = lumaM < lumaLocalAvg;
    bool correctVariation = ((dist1 < dist2 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaMSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    finalOffset = max(finalOffset, subpixC);

    float2 finalUv = uv;
    if (isHorizontal) {
        finalUv.y += finalOffset * stepLength;
    } else {
        finalUv.x += finalOffset * stepLength;
    }

    float3 finalColor = inputTex.Sample(finalUv).rgb;
    return float4(finalColor, 1.0);
}
