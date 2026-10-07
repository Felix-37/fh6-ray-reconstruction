// Specular motion vectors for DLSS Ray Reconstruction (kBufferTypeSpecularMotionVectors).
//
// Derived from speedlemur's Control Ray Reconstruction (specmv.cs_5_0.hlsl, branch control-rr of
// github.com/speedlemur/renodx). Copyright (C) 2026 speedlemur, SPDX-License-Identifier: MIT.
// Changes for Forza Horizon 6 (rr-forza):
//  - Forza stores no reflection hit distance (its SSR / DXR reflection passes only keep the chosen mip,
//    the gloss and a confidence), so the distance is an estimate: a fixed distance set in the menu,
//    scaled by gloss^2 (the same roughness fade Control uses).
//  - The reflected image is treated as static in the world. That is exact for static mirrors (glass,
//    puddles), and for a reflector that moves within its own plane (car panels sliding along the road),
//    whose mirror image of a static object does not move. Control's formula adds the surface's object
//    motion instead, which is right for static mirrors but wrong for the player's car.
//  - Matrices and the motion vector scale come from Streamline's slSetConstants (row-major, row vectors).
//
// The reflected object appears along this pixel's own view ray, pushed back by the reflection distance:
//   virtual = surface + viewDir * t,   t = Distance * gloss^2
//   specMV  = NdcDelta(virtual) + (1 - gloss^2) * (gameMV - NdcDelta(surface))
// At gloss 0 the result is the game's motion vector (the reflection is a blur that moves with the
// surface); at gloss 1 it is the motion of a static point at distance t behind the surface.

#define RS "RootConstants(num32BitConstants=54, b0), DescriptorTable(SRV(t0, numDescriptors=3), UAV(u0, numDescriptors=1))"

cbuffer Constants : register(b0)
{
    row_major float4x4 ClipToView;       // Constants::clipToCameraView
    row_major float4x4 ViewToClip;       // Constants::cameraViewToClip
    row_major float4x4 ClipToPrevClip;   // Constants::clipToPrevClip
    uint Width;
    uint Height;
    float Distance;                      // metres for gloss 1
    uint Flags;                          // reserved
    float MvScaleX;                      // Constants::mvecScale: stored MV * scale = uv delta
    float MvScaleY;
};

Texture2D<uint> Packed : register(t0);   // copy of the RT G-buffer: normal in bits 31..8, gloss in 7..0
Texture2D<float> Depth : register(t1);   // game depth (reverse Z, 0 = sky)
Texture2D<float2> GameMV : register(t2); // game motion vectors (current -> previous)
RWTexture2D<float2> SpecMV : register(u0);

// An ndc-space correction beyond this is not a reflection but a bad matrix or distance (1 = half the
// screen in one frame).
#define NDC_CORRECTION_MAX 1.0

float3 ViewPos(uint2 p, float z)
{
    const float2 uv = (float2(p) + 0.5) / float2(Width, Height);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 v = mul(float4(ndc, z, 1.0), ClipToView);
    return v.xyz / v.w;
}

// Screen motion of a static view-space point, current -> previous, in ndc units.
float2 NdcDelta(float3 view_pos)
{
    const float4 clip_now = mul(float4(view_pos, 1.0), ViewToClip);
    const float4 clip_prev = mul(clip_now, ClipToPrevClip);
    return clip_prev.xy / clip_prev.w - clip_now.xy / clip_now.w;
}

// ndc delta -> the game's motion vector units (uv delta = stored MV * mvecScale).
float2 ToMvUnits(float2 ndc_delta)
{
    return float2(0.5 * ndc_delta.x / MvScaleX, -0.5 * ndc_delta.y / MvScaleY);
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;
    const float2 game_mv = GameMV.Load(int3(id.xy, 0));
    const uint packed = Packed.Load(int3(id.xy, 0));
    const float z = Depth.Load(int3(id.xy, 0));
    const float gloss = float(packed & 0xffu) / 255.0;
    const float g2 = gloss * gloss;
    if (packed == 0 || !(z > 0.0) || g2 <= 0.0)
    {
        SpecMV[id.xy] = game_mv;   // sky, no RT geometry or fully rough: the surface motion
        return;
    }

    const float3 surface = ViewPos(id.xy, z);
    const float3 virtual_pos = surface + normalize(surface) * (Distance * g2);
    const float2 d_virtual = NdcDelta(virtual_pos);
    const float2 d_surface = NdcDelta(surface);
    if (!all(abs(d_virtual - d_surface) < NDC_CORRECTION_MAX) || any(isnan(d_virtual)) || any(isnan(d_surface)))
    {
        SpecMV[id.xy] = game_mv;
        return;
    }
    const float2 object_motion = game_mv - ToMvUnits(d_surface);
    SpecMV[id.xy] = ToMvUnits(d_virtual) + (1.0 - g2) * object_motion;
}
