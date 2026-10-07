// Pass-through replacement for Forza Horizon 6's reflection mip blur
//   SSR_FullScreen_FilterMip_Horizontal (CRC 0x531F30AA) and _Vertical (CRC 0x62D725EE).
// The originals are a separable 9-tap gaussian per channel (no alpha weighting): the centre tap has
// weight 1 and the sum is normalised by the weight sum. Writing only the centre tap reproduces the
// unblurred signal, so DLSS Ray Reconstruction gets the noisy reflections instead of the game's blur.
// Bindings and coordinate math copied from the DXIL (Shader Model 6.6, CreateHandleFromBinding):
//   t0 source, u0 target, s0 sampler, b4 = { uint2 target size }, b3 c51.xy = valid-area scale.

Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);
SamplerState linear_clamp : register(s0);

cbuffer pass_constants : register(b4)
{
    uint2 target_size;
};

cbuffer view_constants : register(b3)
{
    float4 unused[51];
    float4 c51;   // .xy = fraction of the target that holds valid data
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= target_size.x || id.y >= target_size.y)
        return;
    const float2 size = float2(target_size);
    const float2 uv = (float2(id.xy) + 0.5) / size;
    const float2 lo = 0.5 / size;
    const float2 hi = (floor(c51.xy * (size - 1.0)) + 0.5) / size;
    target[id.xy] = source.SampleLevel(linear_clamp, min(max(uv, lo), hi), 0.0);
}
