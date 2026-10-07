# Measurements and discarded ideas

I measured everything here on my PC: RTX 4050 Laptop GPU (6 GB), 1920×1200 output, DLSS at
Quality or Balanced, Multi Frame Generation 4x, 138 fps cap, Forza Horizon 6 6.440.853.0, 2–5 October
2026. Unless stated otherwise, the numbers come from the built-in bench (Measurement tab).

Please read this before proposing an idea: many obvious ones were already tried. If you have a
reason to think one of them deserves another try, say why in the issue.

## How the bench measures

- The camera and the car stay still. Each variant is applied, left to settle (90 DLSS evaluations,
  300 after switching DLSS-SR↔RR or the GI mode) and recorded for 8 frames, one every 6 evaluations
  (~1.5 s, to see the slow "boiling" of the GI).
- Luminance on a log2 scale (16 EV). **Detail** = high-frequency energy of the mean of the frames
  (noise averages out). **Noise** = per-pixel temporal standard deviation. Both per distance band
  (near < 15 m, mid, far 60–250 m, very far), from the depth.
- A control variant (your settings again) runs at the end. **The first RR variant after DLSS-SR is
  often contaminated** (noise ×4–6): trust the control.
- Cars passing in the background ruin the noise numbers of a variant. **Always look at the images**
  (`report.html`: image, temporal noise map, full-resolution crop).
- Limitation: it only measures a still camera. The "Measure disocclusion" button simulates motion by
  dropping RR's and the game's GI history in the same frame and recording frames 1–8 after that.

## Results

### RR with the game's own GI filters ("Game original")

- **Night:** 2–6× less temporal noise than DLSS-SR near the camera, with a large spread between
  scenes and runs (passing cars).
- **Day:** same brightness and medium detail; **−10 to −17 % of the finest detail** at every distance
  (3 scenes). Sharpening (CAS-style, on the HDR output) at **30 %** brings it back to 1.04–1.07× of
  DLSS-SR without more noise; 60 % and 90 % overshoot, and 90 % adds noise. Hence the 30 % default.
- **Tunnels:** the slow boiling of the indirect light drops by ~17 % (Balanced) to ~22 % (Quality),
  but does not go away. It comes from the game's RTGI temporal filter.
- **Thin geometry** (fences, cables) is clearly sharper with RR once the foliage guides stopped
  flagging it.

### Full RR (game RTGI filters replaced, RR denoises the raw GI)

- By day it removes the boiling of the indirect light (my main reason to use it).
- **Night street** (4 Oct 2026, `bench-20261004-175842`): about **40× less temporal noise** than
  DLSS-SR near the camera, same detail, but with 2–4 px dots on walls (this was before the isolated
  sample clamp existed).
- **Fireflies at night** (night facade, `bench-20261005-002047`, noise of the facade): no clamp 7.5 →
  isolated sample clamp k = 4: 4.4, k = 2: 4.6, k = 1: 4.9 (game GI 3.7–3.9, DLSS-SR 5.6), and the dots
  disappear in the images. Bushes 24.6 → ~18–19 (= game GI). Brightness: k = 4 ±0.03 EV, k = 2 −0.05,
  k = 1 −0.07 to −0.13. Firefly suppression v2 on top darkens by −0.31 EV near the camera.
  **Chosen: k = 4, v2 off.** Brightness by day with k = 4 is still to be confirmed.
- **Disocclusion** (error of frames 1→7 against the converged image, in EV, by day,
  `bench-20261005-012655`): game GI 0.178 → 0.091; Full RR 0.262 → 0.142; Full RR + assist 4
  0.194 → 0.102; + assist 8 0.197 → 0.108 (noise floors 0.017–0.023). Disocclusion assist removes
  ~25–30 % of the disocclusion noise; nobody reaches the floor in 8 frames (RR itself needs longer).
- **Disocclusion at night** (`bench-20261005-013631`, older bench version): game GI 0.219 → 0.121
  (the worst), Full RR 0.198 → 0.097, assist 4 0.192 → ~0.12 (odd jump at frames 6–7), assist 8
  0.176 → 0.084 (the best). Converged, assist 8 has half the near noise of Full RR (0.0014 vs 0.0029,
  same as the game GI) without losing detail. **Assist 8 is recommended at night**; by day 4 = 8.
- **Convergence after an RR history reset** (3 night scenes, before the k = 4 clamp): Full RR has
  1.7–1.9× the error of the game GI in each of the 8 frames (frame 1: 0.130 vs 0.077, 0.170 vs 0.106,
  0.169 vs 0.093 EV).
- **Light noise in motion** (no disocclusion) disappears with the game's GI: the cause is the raw GI
  that RR cannot accumulate in motion with little light, not the guides. Hence the automatic hand-over
  to "History x4" while the camera moves (by eye, I prefer x4 over the original there).
- **Car reflections at night:** Full RR ~2× the noise of the game GI (0.0033 vs 0.0015–0.0018) and
  −0.15 EV. By day, still, the bodywork noise is the same in every variant (~0.0007), but the bonnet
  edge has ~2× more noise with Full RR.

### From a gameplay recording (7 Oct 2026)

A 7-minute screen recording (H.264, 30 fps, 1920×1200) in "Full RR" mode, pressing F11 many times.
I matched every press to `ReShade.log` and measured the per-pixel temporal standard deviation of the
8-bit luma (0–255), only in stretches where the car and the camera were still. The video encoder
smooths part of the noise, so these numbers are lower than the bench's and can only be compared with
each other. The stretches are short (0.6–1.9 s): treat them as indications. The clips in the README
come from this recording.

- **Cockpit, forest road, right glove:** RR off 2.33, RR on 0.32–0.51.
- **The boiling with RR off is the game's normal behaviour,** not an effect of having just pressed F11:
  in the tunnel cockpit the right glove gives 3.25 at the start of a 5 s off stretch and 2.94 at its
  end, and in the city cockpit after 26 s off it gives 2.0–2.2.
- **After F11 turns RR on, the image needs about 2–3 s to settle.** In the tunnel (chase camera) the
  wall had more temporal noise than with the game's GI 0.6 s after the press (0.76 vs 0.56) and less
  3 s after it (0.33 vs 0.57). Compare F11 shots only after waiting a few seconds.
- **The cockpit is darker with RR** (glove luma 12 → 8 in the forest). I do not know yet which one is
  closer to the right brightness.
- **Outside the car, by day** (chase camera in the tunnel, city), the still frames show only small
  differences: cleaner indirect light on the tunnel ceiling and walls, slightly softer texture, a bit
  more shading inside tree tops.

### Cost

See the G4 table in [FRAME-MAP.md](FRAME-MAP.md#cost-of-rr-gate-g4-3-oct-2026): RR at Quality costs
+0.9 ms of GPU per presented frame (−6 % fps); RR at Balanced costs the same as DLSS-SR at Quality.
The guides alone cost nothing measurable (GPU 90.7 % vs 90.8 %).

## Discarded (measured, do not propose again without new evidence)

| Idea | Result |
|---|---|
| Exposure texture for RR at −2 / −4 EV (the game passes no exposure) | No effect on burnt tree tops or anything else. Removed from the menu. |
| Sky guide modes: black, white, mirror | Identical. Fixed to white. |
| Foliage options (neutral albedo, dilation) against burnt tree tops | No measurable gain; white foliage albedo cost far detail (fences). Kept, off by default. |
| Firefly suppression v1 (per channel, 3×3, k·σ) and "maximum" | Useless. |
| Firefly suppression v2 (luminance, 5×5) | Removes 60–90 % of the strong dots but darkens (−0.3 EV). Superseded by the isolated sample clamp. |
| Scaling the chroma (Co/Cg) as well in v2, detection by max RGB channel | Worse in 3 walls (more dots, −0.3 EV, blocky stains). Reverted. |
| Absolute radiance cap (16 / 8 / 4 instead of the game's 32) | Useless: dim scenes never reach it (tunnel, `bench-20261004-235508`). |
| Pre-accumulation of 2 / 4 frames before RR (`cap2`, `cap4`) | Noise ×8 (grain). The dot counter looked better, the images did not. |
| RR presets E and D | More noise. F stays. |
| Bypassing the game's spatial RTGI filter alone (F7, first attempt) | Worse: stains and colour shifts. The temporal filter correlates the noise and RR cannot clean it. |
| Bypassing `FilterMip` of the reflections (F8) | Barely any change: Forza's reflections are deterministic (mirror ray + mip pyramid). |
| Metalness from the albedo alpha, specular fade on/off | No measurable effect on tree tops or on the car. Metalness stays opt-in. |
| History x2 / x4 against the slow boiling on night facades and tunnel shadows | No improvement. Probably RT shadows, not GI. |
| Freeing the idle DLSS model (`slFreeResources`) | Saves ~300 MB but breaks switching with F11. |
| Decompiling `RTGI_TemporalFilter` to HLSL (RenoDX decompiler) | "Unsupported loop detected". Patching the DXIL disassembly works instead. |
| RenderDoc | The game closes itself. Use the built-in capture. |

## Bench bugs that were found (and fixed)

- The fence was signalled from ReShade's `execute_command_list` event, which arrives before
  `ExecuteCommandLists`: some frames were read half black. Now signalled after the native call.
- Frames identical to the previous variant appeared after a reset: a copy recorded on a list that
  was never executed left the old data in the readback buffer. Buffers are now zeroed after reading
  and such frames are dropped (counted in "Verification").
- Intermittent GPU hangs: the readback buffer was released while the GPU was still writing it
  (fence signalled too early). Resources are now created when the bench starts, never mid-frame.
