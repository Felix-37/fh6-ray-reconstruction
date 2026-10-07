# Prior art

Research done before starting (3 October 2026): has anyone already brought Ray Reconstruction to
Forza Horizon 6, and how did similar mods do it?

## Forza Horizon 6

- **No official RR.** The PC feature list (forza.net) and NVIDIA's launch news name DLSS Super
  Resolution, DLAA, Frame Generation, Multi Frame Generation and Reflex. Ray tracing = reflections
  (cars and world) + RTGI.
- The Steam forum thread "DLSS 4.5 Ray Reconstruction" for FH6 is a user request, not support.
- Nexus FH6 mod 958 ("Streamline SDK And DLSS 4.5 Updater") only updates DLLs.
- No RR integration for FH6 was found, and RenoDX had no Forza add-on.

## The direct precedent: Control Ray Reconstruction (speedlemur, MIT)

- Nexus Control mod 149. Source: [speedlemur/renodx](https://github.com/speedlemur/renodx), branch
  `control-rr`, `src/games/control-rr/`.
- Same approach as rr-forza: answers the game's DLSS-SR evaluation with DLSS-RR, builds the guides
  from the G-buffer with a compute shader on the game's command list, and falls back to SR on any
  failure.
- Useful differences:
  1. It intercepts **NGX** (`_nvngx.dll`) instead of Streamline.
  2. It **disables the game's denoiser by replacing its shaders by hash** (RenoDX
     `AddRuntimeReplacement`), with edited HLSL (temporal accumulation weight 0). It keeps the game's
     firefly clamp because it is part of the energy calibration of the art.
  3. **Specular motion vectors** (`specmv.cs`): virtual point = surface + view direction × hit
     distance, as a correction on the game's motion vectors. It reduces trails in reflections.
  4. Selectable RR preset; E was the most stable in its tests. `DLSS.Denoise.Mode = 1`.
- "Control Unfiltered" (same author) only removes the denoiser to use DLSS SR presets M/L.

## Other cases (not applicable to FH6)

- Unreal Engine 5 games with RR compiled in but hidden: Mortal Shell II, Oblivion Remastered,
  S.T.A.L.K.E.R. 2, The Blood of Dawnwalker (`r.NGX.DLSS.DenoiserMode=1`, cvar / UE4SS mods).
- Cyberpunk 2077: native RR, which can be forced on with normal ray tracing through its config.
- PureDark (Skyrim, paid): RR with normals reconstructed from depth and a constant material
  (no real G-buffer).
- OptiScaler: only changes DLSS-D in games that already call it.
- "DLSS5 Autopilot" and similar: RR only in games that already ship `nvngx_dlssd.dll`.

## Warning

Several new repositories with names like "1-Click DLSS5" or "RR and DLSS 5 RenoDX for the games …
Windows free download" follow a malware-bait pattern (new accounts, a big download button, no real
code). Do not download them. This project only publishes source code and releases built from it.
