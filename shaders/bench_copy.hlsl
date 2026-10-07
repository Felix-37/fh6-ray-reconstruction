// Bench readback helper (bench.cpp): copies the DLSS output as linear luminance, and the DLSS depth,
// into the add-on's own R32_FLOAT textures. It reads the game's textures in the layouts the game
// already keeps them in at the end of NGX's evaluation (output: UNORDERED_ACCESS, read through a
// typed UAV; depth: SHADER_RESOURCE, read through an SRV), so no barrier ever changes the layout of
// a game resource. Two copies that did (legacy, then enhanced barriers) hung the GPU.

#define RS "DescriptorTable(UAV(u0, numDescriptors=3), SRV(t0, numDescriptors=1)), " \
           "RootConstants(num32BitConstants=5, b0)"

cbuffer Constants : register(b0)
{
    uint2 output_size;
    uint2 depth_size;
    uint copy_depth;
};

RWTexture2D<float4> game_output : register(u0);   // R11G11B10_FLOAT
RWTexture2D<float> out_luminance : register(u1);
RWTexture2D<float> out_depth : register(u2);
Texture2D<float> game_depth : register(t0);       // depth plane, R32_FLOAT_X8X24_TYPELESS

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < output_size.x && id.y < output_size.y)
    {
        const float3 c = game_output[id.xy].rgb;
        out_luminance[id.xy] = dot(c, float3(0.2126, 0.7152, 0.0722));
    }
    if (copy_depth != 0 && id.x < depth_size.x && id.y < depth_size.y)
        out_depth[id.xy] = game_depth[id.xy];
}
