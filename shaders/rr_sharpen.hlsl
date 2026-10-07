// Adaptive sharpening of the DLSS Ray Reconstruction output (sharpen.cpp), applied in HDR before the
// game's tonemap, HUD and post-processing. The bench measured that RR keeps the image structure but
// loses ~12 % of the finest (1-2 px) texture detail at every distance; this pass gives it back.
//
// Contrast-adaptive sharpening in the spirit of AMD FidelityFX CAS: the strength per pixel falls where
// the 3x3 neighbourhood already spans a wide range (edges, highlights), so it does not ring or halo,
// and it never pushes a value outside its neighbours' range. It works on Reinhard-compressed values
// (x / (1 + x)) so the HDR sky cannot dominate, then expands back.
//
// Pass 0 copies the output into the add-on's texture; pass 1 reads that copy and writes the result
// back into the output, in the layout the game keeps it in (UNORDERED_ACCESS): no barrier ever
// changes the output's layout.

#define RS "DescriptorTable(UAV(u0, numDescriptors=2)), RootConstants(num32BitConstants=4, b0)"

cbuffer Constants : register(b0)
{
    uint2 size;
    float sharpness;   // 0..1
    uint pass_index;   // 0 = copy, 1 = sharpen
};

RWTexture2D<float4> game_output : register(u0);   // R11G11B10_FLOAT
RWTexture2D<float4> copy_tex : register(u1);      // R11G11B10_FLOAT (add-on copy)

float3 compress(float3 c) { return c / (1.0 + c); }
float3 expand(float3 t) { return t / max(1.0 - t, 1e-3); }

float3 tap(int2 p)
{
    p = clamp(p, int2(0, 0), int2(size) - 1);
    return compress(max(copy_tex[p].rgb, 0.0));
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;
    const int2 p = int2(id.xy);
    if (pass_index == 0)
    {
        copy_tex[p] = float4(game_output[p].rgb, 1.0);
        return;
    }

    const float3 a = tap(p + int2(-1, -1)), b = tap(p + int2(0, -1)), c = tap(p + int2(1, -1));
    const float3 d = tap(p + int2(-1, 0)), e = tap(p), f = tap(p + int2(1, 0));
    const float3 g = tap(p + int2(-1, 1)), h = tap(p + int2(0, 1)), i = tap(p + int2(1, 1));

    // Soft min/max over the cross plus the full 3x3 (as CAS does).
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, i)));
    mx += max(mx, max(max(a, c), max(g, i)));

    // Amount: high where there is headroom (flat or fine texture), low on strong contrast.
    float3 amp = saturate(min(mn, 2.0 - mx) / max(mx, 1e-5));
    amp = sqrt(amp);
    const float peak = -1.0 / lerp(8.0, 5.0, saturate(sharpness));
    const float3 w = amp * peak;
    float3 t = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
    t = clamp(t, min(min(min(d, e), min(f, b)), h), max(max(max(d, e), max(f, b)), h));
    game_output[p] = float4(expand(min(t, 0.999)), 1.0);
}
