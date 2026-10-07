// Pass-through replacement for Forza Horizon 6's RTGI spatial denoiser
//   RTGI_SpatialFilter_Disk (CRC 0x209AB6A4).
// The original (DXIL, Shader Model 6.6) does, per 8x8 thread tile:
//   1. swizzles the thread id into the output pixel (px, py);
//   2. hashes the pixel to jitter a depth lookup (t1) and writes (0,0,0,1) / 0 when the depth is
//      invalid (depth <= 0, or c0.w / (depth - c0.z) <= 0);
//   3. otherwise starts from the centre samples of t23 and t22 (weight 1), accumulates a disk of
//      neighbours and writes  u3 = filtered t23,  u2 = filtered t22  (normalised by the weight sum).
// This version keeps 1 and 2 bit-exact and writes the centre samples only, so DLSS Ray
// Reconstruction receives the stochastic GI signal instead of the game's spatial blur.

cbuffer rtgi_constants : register(b0, space36)
{
    float4 c[26];   // c[0].zw: depth linearisation; c[25].xy: depth lookup scale
};

Texture2D<float4> depth_tex : register(t1, space36);
Texture2D<float4> gi_b : register(t22, space36);   // -> u2
Texture2D<float4> gi_a : register(t23, space36);   // -> u3
RWTexture2D<float4> out_b : register(u2, space36);
RWTexture2D<float4> out_a : register(u3, space36);

float hash01(uint v)
{
    uint t = (v >> 8) ^ v;
    t += 1759714724u;
    t = (t << 8) ^ t;
    t *= 458671337u;
    const uint r = (t & 16777215u) ^ (t >> 8);
    return float(r) * 5.9604644775390625e-8;   // 2^-24
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint x = id.x, y = id.y;
    const uint px = ((x >> 1) & 2u) | (x & ~6u) | ((y << 1) & 4u);
    const uint py = (y & ~3u) | ((x >> 1) & 1u) | ((y << 1) & 2u);
    const uint2 p = uint2(px, py);

    const uint s = px * 5u + py;
    const float jx = hash01(s + 37u);
    const float jy = hash01(s + 38u);
    const uint2 dp = uint2((jx + float(px)) * c[25].x, (jy + float(py)) * c[25].y);
    const float depth = depth_tex.Load(int3(dp, 0)).x;

    // Same as the original unordered compares (fcmp ugt): NaN counts as valid.
    if (depth <= 0.0 || c[0].w / (depth - c[0].z) <= 0.0)
    {
        out_b[p] = float4(0.0, 0.0, 0.0, 1.0);
        out_a[p] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    out_a[p] = gi_a.Load(int3(p, 0));
    out_b[p] = gi_b.Load(int3(p, 0));
}
