// Firefly-suppressing replacement for Forza Horizon 6's RTGI spatial denoiser
//   RTGI_SpatialFilter_Disk (CRC 0x209AB6A4), used by the "RR completo" GI mode.
// Same thread swizzle, hash and depth early-out as bypass_rtgi_spatial.hlsl (bit-exact with the
// original). Instead of the game's disk blur it writes the centre sample with each channel clamped
// to  mean +- K * sigma  of its 8 valid neighbours (centre excluded). An isolated sample with far
// more energy than its surroundings (a ray that hit a street lamp: an orange dot on a wall at
// night) falls outside that range and is pulled back; smooth variation and real edges stay inside
// it, so DLSS Ray Reconstruction still receives an unblurred, per-frame signal.
// With -D RANGE=1 the clamp is to the [min, max] of the 8 neighbours instead: any sample brighter
// (or darker) than all its neighbours is pulled to the nearest one. The bench showed 2-4 px dots on
// night walls that survived k = 2, because with raw 1-sample GI sigma itself is large.
// With -D LUM5=1 (firefly suppression v2, 5 oct 2026): looks only at the luminance L0 (u3.w) over a
// 5x5 window (the RTGI traces at checkerboard density, so a firefly can cover 2x2 samples); when the
// centre exceeds K times the mean of its valid neighbours it scales the luminance SH down to that limit.
// K is set at compile time (-D K=...): the twin pipeline uses the game's root signature, so it
// cannot take constants of its own.

#ifndef K
#define K 2.0
#endif

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

// GI encoding, read from the RTGI temporal filter's no-history path (rtgi_temporal.orig.ll, lines
// ~1180-1262): radiance -> YCoCg (Y = .25r + .5g + .25b, Co = .5r - .5b, Cg = -.25r + .5g - .25b);
// u3 = (0.5 Y * dir.xyz, 0.5 Y), u2 = (Co, Cg, 0, hit-distance factor).

// Pixels the temporal filter rejected (invalid depth) hold exactly these values.
bool is_invalid(float4 a, float4 b)
{
    return all(a == float4(0.0, 0.0, 0.0, 0.0)) && all(b == float4(0.0, 0.0, 0.0, 1.0));
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

    const float4 ca = gi_a.Load(int3(p, 0));
    const float4 cb = gi_b.Load(int3(p, 0));

    uint w, h;
    gi_a.GetDimensions(w, h);
#ifdef LUM5
    {
        float sum = 0.0, count = 0.0;
        [unroll] for (int dy5 = -2; dy5 <= 2; ++dy5)
            [unroll] for (int dx5 = -2; dx5 <= 2; ++dx5)
            {
                if (dx5 == 0 && dy5 == 0)
                    continue;
                const int2 q = int2(p) + int2(dx5, dy5);
                if (q.x < 0 || q.y < 0 || q.x >= int(w) || q.y >= int(h))
                    continue;
                const float4 na = gi_a.Load(int3(q, 0));
                const float4 nb = gi_b.Load(int3(q, 0));
                if (is_invalid(na, nb))
                    continue;
                sum += abs(na.w);
                count += 1.0;
            }
        float scale = 1.0;
        if (count >= 6.0)
        {
            const float limit = K * max(sum / count, 1e-5);
            const float centre = abs(ca.w);
            if (centre > limit)
                scale = limit / centre;
        }
        // Only the luminance SH is scaled. Scaling the chroma too (and detecting on the brightest RGB
        // channel) was tried on 5 oct 2026 and measured WORSE on 3 night walls: more blinking dots
        // (e.g. >2 EV dots 2472 -> 7171 per MP-frame at k = 2.5), a darker wall (-0.3 EV) and blocky
        // patches at k = 1.6. This luminance-only version cut the strong dots by 60-90 % instead.
        out_a[p] = ca * scale;
        out_b[p] = cb;
        return;
    }
#endif
    float4 sum_a = 0.0, sum_b = 0.0, sq_a = 0.0, sq_b = 0.0;
    float4 min_a = 1e30, max_a = -1e30, min_b = 1e30, max_b = -1e30;
    float n = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
                continue;
            const int2 q = int2(p) + int2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= int(w) || q.y >= int(h))
                continue;
            const float4 na = gi_a.Load(int3(q, 0));
            const float4 nb = gi_b.Load(int3(q, 0));
            if (is_invalid(na, nb))
                continue;
            sum_a += na;
            sum_b += nb;
            sq_a += na * na;
            sq_b += nb * nb;
            min_a = min(min_a, na);
            max_a = max(max_a, na);
            min_b = min(min_b, nb);
            max_b = max(max_b, nb);
            n += 1.0;
        }

    // Too few valid neighbours (silhouettes, screen border): keep the sample as it is.
    if (n < 3.0)
    {
        out_a[p] = ca;
        out_b[p] = cb;
        return;
    }
#ifdef RANGE
    out_a[p] = clamp(ca, min_a, max_a);
    out_b[p] = clamp(cb, min_b, max_b);
    return;
#endif
    const float4 mean_a = sum_a / n, mean_b = sum_b / n;
    const float4 sigma_a = sqrt(max(sq_a / n - mean_a * mean_a, 0.0));
    const float4 sigma_b = sqrt(max(sq_b / n - mean_b * mean_b, 0.0));
    out_a[p] = clamp(ca, mean_a - K * sigma_a, mean_a + K * sigma_a);
    out_b[p] = clamp(cb, mean_b - K * sigma_b, mean_b + K * sigma_b);
}
