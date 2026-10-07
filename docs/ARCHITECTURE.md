# Architecture

How `rr-forza.addon64` brings DLSS Ray Reconstruction to Forza Horizon 6. Read this before changing
code, and read [Rules learned the hard way](#rules-learned-the-hard-way) before touching anything
that runs on the GPU.

## The big picture

```
ForzaHorizon6.exe ──calls──► sl.interposer.dll (Streamline 2.14.1) ──► sl.dlss.dll / sl.dlss_d.dll ──► nvngx_dlss.dll / nvngx_dlssd.dll
        │                         ▲ hooked exports: slInit (manual patch), slSetTagForFrame, slSetConstants,
        │                         │ slEvaluateFeature; slDLSSSetOptions (through slGetFeatureFunction)
        ▼
  D3D12 runtime ◄── hooked native ID3D12GraphicsCommandList / ID3D12CommandQueue methods:
        ▲            ResourceBarrier, Barrier, BeginEvent/EndEvent/SetMarker, SetPipelineState, ExecuteCommandLists
        │
  ReShade 6.8 (dxgi.dll) ──► rr-forza.addon64: add-on events (init_pipeline, present, execute_command_list,
                             draw/dispatch, overlay), settings in ReShade.ini [RR_Forza]
```

The game executable is protected (its code section is encrypted on disk) and is never patched or
read. Every hook lives in NVIDIA's Streamline library or in the D3D12 runtime, the same places other
mods (MFG Unlock, OptiScaler) hook.

## Source map

| File | Role |
|---|---|
| `src/mapper_main.cpp` | Add-on entry point, registration with ReShade, logging and resource description helpers. |
| `src/settings.cpp` | All user settings (`rr::cfg`), their defaults, load/save in `ReShade.ini [RR_Forza]`, `capture_root()`. |
| `src/slinit_hook.cpp` | Adds `kFeatureDLSS_RR` to the game's `slInit` feature list, so Streamline loads the RR plugin. |
| `src/sl_hooks.cpp` | Observes the game's Streamline calls and evaluates DLSS-RR in place of DLSS-SR. |
| `src/guides.cpp`, `shaders/guides.hlsl` | Builds the RR guide buffers from the game's RT G-buffer every frame. |
| `src/replace.cpp` | Swaps the game's RTGI filter pipelines for patched twins (the "Game GI" modes). |
| `scripts/patch_rtgi.pl`, `scripts/patch_rtgi_spatial.pl`, `tools/dxil_asm.cpp` | Build-time patching of the game's DXIL (see [game-shaders/README.md](../game-shaders/README.md)). |
| `shaders/bypass_rtgi_spatial.hlsl`, `shaders/rtgi_spatial_declamp.hlsl` | HLSL replacements of the spatial filter ("pass" and firefly v2 variants). |
| `src/sharpen.cpp`, `shaders/rr_sharpen.hlsl` | Optional CAS-style sharpening of RR's HDR output. |
| `src/bench.cpp`, `src/metrics.cpp`, `shaders/bench_copy.hlsl`, `tools/rr_metrics.cpp` | Measurement bench and its offline re-analysis tool. |
| `src/capture.cpp`, `src/image.cpp`, `src/marker_hooks.cpp`, `src/shader_dump.cpp` | Frame capture (F10), native barrier/marker hooks, shader hashing and export. |
| `src/overlay.cpp` | The "Ray Reconstruction" tab of the ReShade overlay (English and Spanish). |
| `experiments/` | Archived experiments (bypasses F7/F8 of 3 Oct 2026), not built. |

## Lifetime and hooking

- ReShade loads a **temporary instance** of every add-on at start and unloads it about 20 s later.
  No hot hook may live in that instance: hooks are installed only after 120 real presents
  (`hooks_allowed()`), every detour counts itself in `g_inflight`, and `shutdown_hooks()` waits for
  zero.
- The **slInit hook** is the exception: the game calls `slInit` milliseconds after loading
  `sl.interposer.dll`, before any present. A loader notification (`LdrRegisterDllNotification`) runs
  synchronously while the DLL loads and patches `slInit` with a jump to a small executable page that
  is never freed. The page jumps to our hook, then to a trampoline after `slInit` returns, so code
  that other mods relocate or restore later never points into an unloaded module. The patch expects
  the prologue of Streamline 2.14.1 (`mov [rsp+10h], rdx`) or an existing `jmp`; anything else is
  logged and left alone.
- Streamline exports are hooked with **MinHook**, which chains with detours from other mods.

## 1. Loading the RR plugin

The game passes a closed `featuresToLoad` list to `slInit` without `kFeatureDLSS_RR`, so
`sl.dlss_d.dll` is never loaded. `slinit_hook.cpp` copies the list, appends `kFeatureDLSS_RR` and
calls the original. Later, `setup_ray_reconstruction()` checks `slIsFeatureLoaded(kFeatureDLSS_RR)`
and gets `slDLSSDSetOptions` through `slGetFeatureFunction`.

## 2. What the game gives DLSS

From the hooked `slSetTagForFrame` / `slSetConstants` / `slDLSSSetOptions`
(details in [FRAME-MAP.md](FRAME-MAP.md)):

- `ScalingInputColor`: R11G11B10F HDR, before tonemapping, extent = render resolution.
- `ScalingOutputColor`: R11G11B10F at output resolution. `Depth`: D32S8 (reverse-Z).
  `MotionVectors`: RG16F. No exposure texture.
- Full camera constants: matrices, camera position and basis, near/far, jitter.
- `DLSSOptions`: mode, output size, `preExposure`, `exposureScale`, HDR flag.

The add-on records the render extent, the output and depth resources (for the bench), the camera
basis (for the guides and the RR matrices) and the camera motion per frame (for the motion hand-over).

## 3. Guide buffers

DLSS-RR needs to know the surfaces it is denoising. Forza renders Forward+, but it also draws a
reduced G-buffer for its ray tracing effects in the pass `RTBufferEffectsAndFPlusPlus`:

| Game texture (render resolution) | Content |
|---|---|
| `RGBA8_SRGB` | Albedo (unlit colour). Alpha = 255 on car bodywork, 0 elsewhere: probably metalness, unconfirmed. |
| `R32_UINT` | Bits 31–8: octahedral world normal, 12 + 12 bits, Y up, axes `(oy, ox, oz)`. Bits 7–0: gloss. |

`guides.cpp` finds both by watching the game's **enhanced barriers** (`RENDER_TARGET` →
`SHADER_RESOURCE` at the end of that pass), copies them into add-on textures with a layout round trip
that mirrors the game's own barrier, and dispatches `shaders/guides.hlsl` on the **native** command
list, inside the NGX evaluation marker, right before DLSS runs. Outputs (render resolution):

- **Normal + roughness** (`RGBA16F`, `DLSSDNormalRoughnessMode::ePacked`): roughness = 1 − gloss
  (assumption).
- **Diffuse albedo** (`RGBA8`): albedo × (1 − metalness). Metalness is 0 unless "Metalness from the
  albedo alpha" is on.
- **Specular albedo** (`RGBA8`): Karis' analytic split-sum environment BRDF with F0 = 0.04 (or the
  albedo for metals), using N·V from the camera basis.
- **Sky / no geometry**: white diffuse and no specular, so RR treats those pixels like plain upscaling.
- **Foliage**: the RT G-buffer holds a sparse proxy of the foliage with random normals. The coherence
  of each normal with its 4 neighbours drives an optional specular fade, an optional neutral (white or
  floored) diffuse albedo and an optional 5×5 mask dilation.
- **Diffuse hit distance** (`R16F`, optional): copied from the game's final GI (output of the RTGI
  upscale, pipeline CRC `0x14FA42AB`), whose `w` is a normalised hit distance. Distance = scale × w²,
  with scale = 10 m by default (fitted on captures: w ≈ √(distance / 10 m)).

The guides stay in `NON_PIXEL_SHADER_RESOURCE` between frames, which is the state they are tagged
with, so Streamline never transitions them mid-evaluation.

## 4. Evaluating RR instead of DLSS-SR

`evaluate_hook()` intercepts `slEvaluateFeature(kFeatureDLSS)`. `try_ray_reconstruction()` uses RR
only when everything is ready, otherwise the frame goes to the game's own DLSS-SR:

- RR enabled and loaded, DLSS options and the camera basis seen;
- guides built from a **fresh G-buffer copy of this frame**, at exactly the DLSS input size (menus,
  loading screens and Alt+Tab change the G-buffer size: stale or resized guides poisoned RR's history
  once);
- if the previous frame was RR, the guide shader must have run inside its evaluation; if not, RR is
  disabled for the session (it would read stale guides).

It then calls `slDLSSDSetOptions` (mode, size and exposure copied from the game's `DLSSOptions`,
packed normal/roughness, the chosen preset for every quality mode, `worldToCameraView` and
`cameraViewToWorld` built from the camera basis as row-vector matrices), tags the guides and
evaluates `kFeatureDLSS_RR` with the game's own inputs. RR's history is reset after a switch, a gap of
more than 250 ms, a resize, a preset change or a request from the menu. Three failures in a row
disable RR for the session.

## 5. Replacing the game's RTGI filters ("Game GI" modes)

The game denoises its ray traced GI with its own compute passes:

| Pipeline (CRC32 of the shader) | Name in the shader's debug info | Role |
|---|---|---|
| `0x4DAF8A48` | `RTGI_TemporalFilter` | 3×3 resolve of the raw samples, then temporal accumulation with a per-pixel history count |
| `0x209AB6A4` | `RTGI_SpatialFilter_Disk` | Disk filter whose radius grows for pixels with a young history |
| `0x14FA42AB` | (RTGI upscale) | Upscales the GI to render resolution; `w` = normalised hit distance |

`shader_dump.cpp` hashes every pipeline the game creates (ReShade `init_pipeline`, CRC32 as in
RenoDX). When a target appears, `replace.cpp` creates one **twin pipeline per variant** with the
game's root signature, and a hook on the native `SetPipelineState` swaps the original for the twin
the settings select. Mode 0 never swaps anything.

The temporal variants are the game's own DXIL with a few instructions changed. At build time:
`dxc -dumpbin` disassembles the exported shader, a Perl script edits it at exact anchor lines (the
build stops if an anchor is not found exactly once), and `tools/dxil_asm.cpp` reassembles it with
`IDxcAssembler`, copies the root signature (`RTS0`) and debug name (`ILDN`) parts from the original
container and signs it with `IDxcValidator`. Key facts of `RTGI_TemporalFilter`:

- history cap `N = umin(cb0[21].w + 1, previous count)` at `%2246` (variants `hist2`/`hist4` multiply
  it, `cap2`/`cap4` limit it);
- blend weight with the history at `%2260` (`nofilter` sets it to 0: current frame only);
- previous history count at `%2243` (`_reset` variants read it as 0 for one frame, used by the
  disocclusion bench and by the motion hand-over);
- the 3×3 resolve reads the raw samples from group shared memory (`sh_Radiance_hitT`), weighted by
  normal and depth similarity. `nofilter_iso*` clamps the brightest of the 9 samples to k × the second
  brightest **before** this resolve, which is what finally separated fireflies from real light;
- every sample is capped at r + g + b ≤ 32 (`saturate(sum / 32) * 32`).

The spatial filter reads the history count of each pixel (`t28`) and widens its disk for young pixels
(radius = lerp(`cb20.w`, `cb20.z`, saturate((n − 1) / `cb23.z`))). The `assist4`/`assist8` variants
keep the game's filter only for young pixels and fade it out over 4 or 8 frames.

Mode → variants (`desired()` in `replace.cpp`):

| Mode | Temporal filter | Spatial filter |
|---|---|---|
| 0 Game original | original | original |
| 1 / 2 History x2 / x4 | `hist2` / `hist4` | original |
| 3 No temporal filter (RR on) | `nofilter` | original |
| 4 Full RR (RR on) | `nofilter` or `nofilter_iso4/2/1` (isolated sample clamp) | `pass`, `assist4/8` or firefly v2 |
| 4 while the camera moves | `hist4` (one `_reset` frame at the start) | original |

## 6. Sharpening

`sharpen.cpp` runs right after the RR evaluation on the native list NGX used: it copies the HDR
output into an add-on texture, then writes a CAS-style sharpened result back into the output. The
output is only accessed through a UAV in the layout the game keeps it in during DLSS, and only global
barriers order the passes.

## 7. Measurement bench

The "Measurement" tab runs a fixed sequence of variants (DLSS-SR first as the reference, then RR
with different settings) with the camera still. For each variant it applies the settings, lets
everything settle (90 DLSS evaluations, 300 after switching DLSS-SR↔RR or the GI mode), then records
8 frames, one every 6 evaluations. A compute shader (`bench_copy.hlsl`) reads the DLSS output (as
luminance) and the depth **in the layouts the game already has them in** and writes add-on textures,
which are copied to readback buffers. A fence is signalled right after the native
`ExecuteCommandLists` of the list holding the copies. `metrics.cpp` then computes, per distance band
(from the depth): fine and medium detail of the mean image, temporal noise, brightness, sky
contamination of edges, isolated flickering dots, fireflies and, for the disocclusion bench, the error
of frames 1–8 after dropping all history. It writes `report.txt`, `report.csv`, `report.html` and
images. `tools/rr_metrics.exe <folder>` re-runs the analysis offline from the saved frames.

## 8. Frame capture

F10 (developer hotkeys) records one frame: every pass (PIX markers), render target binding, pipeline
(with its shader CRC), draw/dispatch, Streamline call and barrier, and copies every texture that
leaves a write state into readback buffers (`frame-map.txt`, `.dds` and `.png` per texture).
Shift+F10 also copies textures that need a layout round trip; Ctrl+Shift+F10 only captures inside
`RenderRTBufferEffects` and dumps the shaders used there. RenderDoc cannot be used: the game exits
when it detects it.

## Rules learned the hard way

Each of these cost a crash, a GPU hang or corrupted rendering at some point:

1. **Never change the layout of a game resource.** A legacy `ResourceBarrier` on a game resource
   caused "device removed"; even correct enhanced barriers hung the GPU after a few copies. Read game
   resources with a compute shader in the layout they already have (UAV for the DLSS output, SRV for
   the depth) and use global barriers only for synchronisation.
2. **Never call methods on resources obtained through ReShade's view tracking.** ReShade keeps the
   pointer from when the view was created. If the resource was freed, the pointer belongs to another
   object and a virtual call runs a foreign method (white sky and broken textures until restart). Use
   the resources from barriers and Streamline tags, and protect COM calls with SEH when in doubt.
3. **Use the native device from a native command list.** `ID3D12Resource::GetDevice()` on a game
   resource returns ReShade's proxy device, whose descriptor heaps crash a native `SetDescriptorHeaps`.
4. **ReShade's `execute_command_list` event fires before `ExecuteCommandLists`.** Signal fences after
   the native call, or you will read half-written frames.
5. **ReShade's `barrier` event loses enhanced-barrier layouts, subresources and split flags.** Use the
   native `Barrier` / `ResourceBarrier` hooks (vtable slots 80 / 26).
6. **No hot hook in the temporary add-on instance** (see Lifetime above).
7. **Read the DLSS output after `slEvaluateFeature` returns**, not at the end of the NGX marker: the
   RR output was not complete there yet.
8. **`slFreeResources` breaks switching models** with F11 (black DLSS-SR, RR drawn in a corner). Both
   models stay loaded.
9. **`Map` with `RowPitch × rows` overruns** the buffer when the width is not a multiple of 256 bytes.
   Use the footprint of the last row.
10. **Look at the images, not only at the numbers.** A dot counter once called a variant with eight
    times more grain "better".
