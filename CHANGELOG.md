# Changelog

## Unreleased

- Default GI mode is now **History x4** (the most stable image with good quality). Full RR stays
  available as an experimental mode: the best image by day, with more open problems.
- Specular motion vectors for RR (Guides tab, experimental, off by default): reflections are reprojected
  as a static image at an estimated distance (default 30 m, scaled by gloss²), tagged as
  `kBufferTypeSpecularMotionVectors`. Forza stores no reflection hit distance, so the distance is a
  setting. Method adapted from speedlemur's Control Ray Reconstruction (MIT). Not measured yet.

## 0.1.0-beta (2026-10)

First public release.

- DLSS Ray Reconstruction in place of the game's DLSS Super Resolution, with automatic fallback to
  DLSS-SR on any frame where something is missing. F11 toggles it.
- Guide buffers built every frame from the game's RT G-buffer: normals + roughness, diffuse and
  specular albedo, and (optional) diffuse hit distance from the game's final GI.
- "Game GI" modes that replace the game's RTGI denoising filters with patched versions, including
  "Full RR" (default), the isolated sample clamp against fireflies, disocclusion assist and the
  automatic hand-over to history x4 while the camera moves.
- Optional CAS-style sharpening of RR's output (on at 30 % by default).
- Built-in measurement bench (RR vs DLSS-SR, fireflies, car reflections, disocclusion) and offline
  re-analysis tool.
- Overlay in English and Spanish.
- Developer tools: frame capture, game shader export (for building), developer hotkeys (off by
  default).
- Captures and measurements go to `rr-forza-captures` next to the game (`CaptureDir` to change it).
