// ImGui vertex shader for D3D12
// Vulkan NDC: Y 向下 (-1=top, +1=bottom)
// D3D12 NDC: Y 向上 (-1=bottom, +1=top)
// 需要翻转 Y 轴
cbuffer PushConstants : register(b0)
{
    float2 uScale;
    float2 uTranslate;
};

struct VSInput
{
    float2 aPos   : TEXCOORD0;
    float2 aUV    : TEXCOORD1;
    float4 aColor : TEXCOORD2;
};

struct VSOutput
{
    float4 Pos   : SV_POSITION;
    float4 Color : COLOR0;
    float2 UV    : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float2 pos = input.aPos * uScale + uTranslate;
    pos.y = -pos.y;  // 翻转 Y 轴: Vulkan NDC → D3D12 NDC
    output.Pos = float4(pos, 0, 1);
    output.Color = input.aColor;
    output.UV = input.aUV;
    return output;
}
