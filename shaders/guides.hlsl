// Builds the DLSS Ray Reconstruction guide buffers from Forza Horizon 6's RT G-buffer
// (pass RTBufferEffectsAndFPlusPlus, render resolution):
//   t0 = albedo copy, RGBA8_UNORM_SRGB (the SRV decodes to linear)
//   t1 = packed copy, R32_UINT: octahedral world normal 12+12 in bits 31..8, gloss in bits 7..0
// Outputs (render resolution):
//   u0 = normal (world space) + roughness, RGBA16F (DLSSDNormalRoughnessMode::ePacked)
//   u1 = diffuse albedo, RGBA8_UNORM
//   u2 = specular albedo, RGBA8_UNORM
//   u3 = diffuse hit distance, R16_FLOAT (flag 128), from t2 = copy of the game's final GI (RGBA16F,
//        w = normalised hit distance; captures 20261003-192215: w ~ sqrt(hit / 10 m), so hit = scale * w^2)
// Decoding verified against captures/20261003-010233 (docs/frame-map.md). Roughness = 1 - gloss is an
// assumption; the albedo alpha looks like metalness (grey on car paint, white on chrome) but is
// unconfirmed, so it is opt-in (F7).
// Guides v2 (3 oct 2026): white diffuse / zero specular where there is no geometry, so RR leaves the
// sky and far background undemodulated; specular faded out where the normal is noise (foliage).
// Guides v3: diffuse also neutral (white) on that foliage.

#define RS "RootConstants(num32BitConstants=16, b0), " \
           "DescriptorTable(SRV(t0, numDescriptors=3), UAV(u0, numDescriptors=4))"

cbuffer Constants : register(b0)
{
    float4 cam_right_projx;   // xyz = camera right (world), w = cameraViewToClip[0][0]
    float4 cam_up_projy;      // xyz = camera up (world),    w = cameraViewToClip[1][1]
    float4 cam_fwd;           // xyz = camera forward (world), w = foliage threshold (mean neighbour dot)
    uint4 size_flags;         // x, y = render size; z = metal mode (0 = dielectric, 1 = albedo alpha) | hit scale in cm << 8;
                              // w = flags: 1 sky neutral, 2 foliage diffuse neutral, 4 foliage specular fade,
                              //            8 foliage mask dilation (5x5), 16 diagnostic: neutral guides everywhere,
                              //            32 foliage "albedo floor" instead of white; 64 sky as mirror; 128 diffuse hit distance;
                              //            bits 16..23 = floor in percent
};

Texture2D<float4> albedo_tex : register(t0);
Texture2D<uint> packed_tex : register(t1);
Texture2D<float4> gi_tex : register(t2);
RWTexture2D<float4> out_normal_roughness : register(u0);
RWTexture2D<float4> out_diffuse : register(u1);
RWTexture2D<float4> out_specular : register(u2);
RWTexture2D<float> out_hit : register(u3);

float3 oct_decode(float2 e)
{
    e = e * 2.0 - 1.0;
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

float3 decode_normal(uint packed)
{
    const float3 o = oct_decode(float2((packed >> 20) & 0xfff, (packed >> 8) & 0xfff) / 4095.0);
    return float3(o.y, o.x, o.z);
}

// Karis' analytic approximation of the split-sum environment BRDF.
float3 env_brdf_approx(float3 f0, float roughness, float n_dot_v)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size_flags.x || id.y >= size_flags.y)
        return;

    // Diffuse hit distance for RR (world units). The game stores it normalised in the final GI's w; sky
    // and misses read 1 = far. Written before any early return.
    if (size_flags.w & 128u)
    {
        const float w = saturate(gi_tex[id.xy].w);
        out_hit[id.xy] = float((size_flags.z >> 8) & 0xffffu) * 0.01 * w * w;
    }

    const uint packed = packed_tex[id.xy];
    const float4 albedo = albedo_tex[id.xy];

    // View vector from the pixel position alone: the camera sits at the origin of view space.
    const float2 ndc = float2((id.x + 0.5) / size_flags.x * 2.0 - 1.0, 1.0 - (id.y + 0.5) / size_flags.y * 2.0);
    const float3 ray = ndc.x / cam_right_projx.w * cam_right_projx.xyz + ndc.y / cam_up_projy.w * cam_up_projy.xyz + cam_fwd.xyz;
    const float3 v = -normalize(ray);

    if (packed == 0)
    {
        // No geometry (sky, far background): white diffuse and no specular, so dividing the colour by
        // the albedo is the identity and RR treats these pixels like plain upscaling.
        // Flag 64: the sky as a perfect mirror (specular albedo 1, roughness 0, no diffuse), so RR keeps
        // its light in the specular channel instead of the diffuse one it shares with the leaves.
        const bool mirror = (size_flags.w & 64u) != 0;
        out_normal_roughness[id.xy] = float4(v, mirror ? 0.0 : 1.0);
        out_diffuse[id.xy] = (!mirror && (size_flags.w & 1u)) ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.0, 0.0, 0.0, 1.0);
        out_specular[id.xy] = mirror ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // World space, Y up. Mapping checked against the camera basis on surfaces facing the camera,
    // the road, the ceiling and a side wall (captures 20261003-014211): plain x/y swap.
    const float3 n = decode_normal(packed);

    // Normal coherence with the 4 neighbours: foliage in the RT G-buffer has per-pixel random
    // normals, which made RR keep sparkling highlights on leaves. Fade the specular guide there.
    // Silhouettes lose specular on a 1-pixel line only.
    float coherence = 0.0;
    const int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    [unroll] for (int i = 0; i < 4; ++i)
    {
        const int2 q = clamp(int2(id.xy) + offsets[i], int2(0, 0), int2(size_flags.xy) - 1);
        const uint pq = packed_tex[q];
        coherence += pq == 0 ? 1.0 : saturate(dot(n, decode_normal(pq)));
    }
    float specular_scale = (size_flags.w & 4u) ? saturate((coherence * 0.25 - 0.5) / 0.4) : 1.0;
    // Guides v3: the RT G-buffer holds a sparse proxy of the foliage that does not match the dense
    // foliage on screen, so a sky gap could be demodulated by a dark leaf albedo (colour / 0.03) and
    // RR spread that over the leaves (burnt, thin-looking trees). Blend the diffuse guide to white
    // there, like the sky, so RR treats foliage close to plain upscaling.
    float foliage = (size_flags.w & 2u) ? saturate((cam_fwd.w - coherence * 0.25) / 0.3) : 0.0;

    // Optional dilation: leaves drawn as flat cards have a uniform normal inside each card, so the
    // 4-neighbour test misses their interior. Over a 5x5 window the mean agreement with the centre
    // normal drops wherever cards of different orientation meet, which covers small cards entirely.
    // Side effect: a 2-pixel band along sharp geometry edges also counts as foliage (plain upscaling).
    if ((size_flags.w & 2u) && (size_flags.w & 8u))
    {
        float wide = 0.0;
        [unroll] for (int dy = -2; dy <= 2; ++dy)
            [unroll] for (int dx = -2; dx <= 2; ++dx)
            {
                if (dx == 0 && dy == 0)
                    continue;
                const int2 q = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(size_flags.xy) - 1);
                const uint pq = packed_tex[q];
                wide += pq == 0 ? 1.0 : saturate(dot(n, decode_normal(pq)));
            }
        const float foliage_wide = saturate((cam_fwd.w - wide / 24.0) / 0.3);
        foliage = max(foliage, foliage_wide);
        if (size_flags.w & 4u)
            specular_scale = min(specular_scale, 1.0 - foliage_wide);
    }

    // Diagnostic: no albedo demodulation anywhere (RR sees white diffuse and no specular).
    if (size_flags.w & 16u)
    {
        foliage = 1.0;
        specular_scale = 0.0;
    }
    const float roughness = 1.0 - (packed & 0xff) / 255.0;
    const float metal = (size_flags.z & 255u) == 1 ? albedo.a : 0.0;

    const float n_dot_v = saturate(dot(n, v));

    const float3 base = albedo.rgb;
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), base, metal);
    out_normal_roughness[id.xy] = float4(n, roughness);
    // Foliage treatment. White removes the albedo division entirely but also hands the texture
    // detail to RR's denoiser (thin far-away patterns got smoothed); the floor only lifts dark
    // albedo, which is what made sky gaps explode (colour / 0.03), and keeps bright detail intact.
    const float3 diffuse = base * (1.0 - metal);
    const float albedo_floor = float((size_flags.w >> 16) & 255u) * 0.01;
    float3 neutral = (size_flags.w & 32u) ? max(diffuse, albedo_floor.xxx) : float3(1.0, 1.0, 1.0);
    if (size_flags.w & 16u)
        neutral = float3(1.0, 1.0, 1.0);   // the diagnostic is always plain white
    out_diffuse[id.xy] = float4(lerp(diffuse, neutral, foliage), 1.0);
    out_specular[id.xy] = float4(saturate(env_brdf_approx(f0, roughness, n_dot_v)) * specular_scale, 1.0);
}
