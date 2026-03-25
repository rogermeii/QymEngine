// ImGui pixel shader (HLSL equivalent of imgui_impl_vulkan.cpp GLSL)
Texture2D FontTexture : register(t0);
SamplerState FontSampler : register(s0);

struct PSInput
{
    float4 Pos : SV_POSITION;
    float4 Color : COLOR0;
    float2 UV : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    return input.Color * FontTexture.Sample(FontSampler, input.UV);
}
