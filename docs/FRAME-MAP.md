# Frame map of Forza Horizon 6

What a frame of Forza Horizon 6 (PC, DirectX 12, version 6.440.853.0) looks like from the outside,
reconstructed with the add-on's own frame capture (F10 / Shift+F10 / Ctrl+Shift+F10) in October 2026.
Setup of those captures: 1920×1200 output, DLSS (render 1280×800 at Quality, 1114×696 at Balanced),
DLSS Frame Generation 4x, `RTGIQuality=3`, `RTReflectionQuality=1` (3 for the reflection analysis).
Capture names (`20261003-010233`…) refer to my local capture folders.

## General

- Barrier API: **enhanced barriers** (layouts 2 = RENDER_TARGET, 3 = UNORDERED_ACCESS,
  4 = DEPTH_STENCIL_WRITE, 6 = SHADER_RESOURCE). Only ~170 legacy `ResourceBarrier` against ~2900
  `Barrier` per frame.
- Readable PIX markers (PIX3, UTF-16 text from the 4th qword).
- **Forward+** renderer (`ForwardPlus`, `FPlusPlusComposite`) with a reduced G-buffer for the ray
  tracing effects (`RTBufferEffectsAndFPlusPlus`, `CPresentationView::RenderRTBufferEffects`).
- The shaders carry their source name in their debug info (`dxc -dumpbin` shows `shader debug name`),
  which identifies them (`SSR_FullScreen_*`, `RTGI_TemporalFilter`, …).
- RenderDoc cannot be used: the game closes itself when it detects it.

## What the game gives Streamline

- `ScalingInputColor` = R11G11B10F 1920×1200, extent 1280×800 (HDR, before the tonemap).
- `ScalingOutputColor` = R11G11B10F 1920×1200.
- `Depth` = R32G8X24_TYPELESS 1280×800 (D32S8).
- `MotionVectors` = RG16F 1280×800.
- `Exposure` = null.
- `HUDLessColor` and `UIColorAndAlpha` RGBA8 1920×1200 (for DLSS Frame Generation).
- Constants: reverse Z (near 0.1, far almost infinite), `slSetConstants` fully filled.
- `slEvaluateFeature(kFeatureDLSS)` already returns **`eWarnOutOfVRAM`** on a 6 GB GPU, without RR.
- The executable imports `slDLSS*`, `slDLSSG*`, `slEvaluateFeature`, `slSetTagForFrame`, but no
  `slDLSSD*`. `sl.dlss_d.dll` (2.14.1) and `nvngx_dlssd.dll` (310.9.x) are still in the game folder.

## RT G-buffer (gate G1, capture `20261003-010233`, Shift+F10)

Everything in `RTBufferEffectsAndFPlusPlus`, 1280×800, same frame:

- **Albedo** = `RGBA8_SRGB`. Unlit colours (red paint 200, 0, 35). Alpha = mask (255 on the car body,
  0 elsewhere): probably metalness / car paint, unconfirmed.
- **Normals** = `R32_UINT`: **octahedral 12 + 12 bits in bits 31–8** (x = bits 31–20, y = bits 19–8,
  in [0, 1] → octahedral). **World space, Y up**; axes **`n = (oy, ox, oz)`** (a first guess
  `(-oy, ox, oz)` was wrong for surfaces facing the camera).
- **Bits 7–0 of the same texture** = greyscale map with texture detail (car paint ~255, asphalt in the
  middle): taken as **gloss = 1 − roughness** (assumption).
- `RGBA8_UINT`: R = material ID (0–18), G/B/A per-material parameters. `RGBA8_UNORM`: mask/parameters
  (G = 1 almost everywhere, car grey). Two more `R32_UINT`: sparse layers (road and car only),
  probably decals / clear coat.
- Depth, motion vectors and HDR colour before the tonemap: the same the game already tags for DLSS.
- Not present: specular albedo (computed from albedo + metalness + N·V + roughness) and specular hit
  distance.
- **Foliage** in this G-buffer is a sparse proxy (dots with holes, noisy normals), different from the
  dense foliage on screen. That is why the guides treat incoherent normals specially.
- Deferred decals (`Deferred Decals (Track)`, 3 RTs): `RGBA8_SNORM` + 2× `RGBA8_UNORM` 1280×800. The
  main forward pass writes 8 render targets at once.
- When the window loses focus (Alt+Tab), the game switches the G-buffer to 1920×1200 and stops
  evaluating DLSS.

Guides built from it (gate G3, capture `20261003-015108`): |n| within 1 % of 1 on **100 %** of the
pixels with geometry, identical to the reference decode (max difference 0.0005). Axes checked against
the camera: back of the car (0.82, 0.56, 0.13) against −forward (0.98, 0.09, 0.16); road (0.20, 0.98,
0.05); right wall (0.13, 0.23, −0.97) against −right (0.16, 0, −0.99); ceiling (0, −1, 0.04).
Specular albedo: mean 0.08, max 0.89, dark when facing and brighter at grazing angles (Fresnel), as
expected.

## Ray traced global illumination (RTGI)

Inside `CPresentationView::RenderRTBufferEffects`, one `DispatchRays` per frame belongs to the RTGI
(spherical harmonics plus an irradiance cache). Its filters:

| Pass | Pipeline CRC32 | Notes |
|---|---|---|
| `RTGI_TemporalFilter` | `0x4DAF8A48` | 2947-line DXIL. Reduced resolution (coordinates × `cb0[25].xy`). Resolves the raw samples in 3×3 from group shared memory (`sh_Radiance_hitT`, weights from normal × depth), then accumulates over time with a per-pixel history count. GI stored YCoCg-like: `u3 = (0.5·Y·dir, 0.5·Y)`, `u2 = (Co, Cg, 0, hit-distance factor)` (the chroma is absolute). |
| `RTGI_SpatialFilter_Disk` (and a bilateral variant) | `0x209AB6A4` | Reads the history count (`t28`) and widens its disk for young pixels. |
| `rtgi_upscale` | `0x14FA42AB` | Upscales to render resolution. Output RGBA16F: rgb = GI, w = normalised hit distance (≈ √(distance / 10 m)). |

The game feeds its history back with the output of the spatial filter (the textures rotate between
4), so the raw GI and an accumulated history cannot both be handed to RR at the same time.

## Ray traced reflections (gate for "phase 5", 3 Oct 2026)

They only appear with RT reflections on, inside `CPresentationView::RenderRTBufferEffects`
(focused capture `20261003-173853`, at night next to street lamps):

- 8×8 tile classification (`R8_UINT` 140×87) + `ExecuteIndirect`: **inline ray queries** only where
  there are glossy surfaces (no `DispatchRays`).
- **Reflection `RGBA8_UNORM` 1114×696 with 11 mips**: RGB = reflected colour, A = mask of where a ray
  was traced. On the car body it is a sharp, clean mirror-like reflection; on the road it is
  **scattered dots** (stochastic sampling).
- `RGBA8_SNORM` 1114×696 and a 557×348 copy: ray direction / normal; `R8_UNORM` 557×348: mask/weight.
- Mip chain 1–10 + 1/8-resolution buffers (`RGBA8_UNORM` 139×87): the roughness-based reconstruction
  and blur. This is the "denoiser": it fills the gaps between the scattered dots.
- The reflection shaders are `SSR_FullScreen_*` (hybrid screen space + DXR, checkerboard).
  `FilterRoughChosenMip_H/V` (`0xE0472AF3` / `0x06829E83`) filter the mip-selection map, not the
  colour. `FilterMip_H/V` (`0x531F30AA` / `0x62D725EE`) are 9-tap Gaussians.

Conclusion at the time: on the car body the reflection is already clean; on the road the signal is
scattered samples that the game reconstructs with the pyramid. For RR to clean them, the way the
shading reads the pyramid would have to change (game shaders): high risk, small gain. Bypassing
`FilterMip` (experiment F8, `experiments/`) barely changed anything.

## Cost of RR (gate G4, 3 Oct 2026)

RR active during the measurements. PresentMon, 60 s runs, 138 fps cap:

| | fps | Latency | GPU busy | GPU busy per present |
|---|---|---|---|---|
| Baseline, DLSS-SR Quality (3 runs) | 138.1 | 21.1 ms | 90.8 % | 6.58 ms |
| Guides only, no RR (3 runs) | 138.0 | 20.9 ms | 90.7 % | 6.57 ms |
| **RR, Quality (2 runs)** | **130.2** | **25.8 ms** | **97.1 %** | **7.47 ms** |
| RR, Balanced | 138.3 | 19.9 ms | 88 % | – |

VRAM in game with RR: median 5.54 GB, p90 5.79 GB, max 5.88 GB (6 GB card). Releasing the idle model
saves ~300 MB but breaks switching with F11, so both models stay loaded.
