# Open problems

This is where help is most welcome. Each problem lists what is seen, what was measured, what was
already tried and the best current hypothesis. Measurements: [MEASUREMENTS.md](MEASUREMENTS.md).
How the add-on works: [ARCHITECTURE.md](ARCHITECTURE.md). If you work on one of these, open an issue
first so that efforts do not overlap.

## 1. Raw noise in motion and on disocclusion (Full RR)

**Seen:** when moving, or when something comes back on screen, raw GI noise is visible for a few
frames until RR cleans it.

**Measured:** after dropping all history, Full RR has ~1.5× the error of the game's own GI filters in
every one of the first 8 frames. "Disocclusion assist" (the game's spatial filter only on young pixels,
fading out over 4 or 8 frames) removes ~25–30 % of it. Light noise in motion without disocclusion
disappears with the game's GI: RR cannot accumulate the raw GI in motion with little light.

**Already in place:** disocclusion assist (default 4 frames; 8 measured better at night) and the
automatic hand-over to the game's filters with history x4 while the camera moves.

**Why it is hard:** the game's spatial filter reads the GI history count (`t28`) and widens its disk
for new pixels. In Full RR that filter is bypassed, so new pixels reach RR raw. The game feeds its
history back with the spatially filtered output, so the raw GI and an accumulated history cannot be
handed to RR at the same time.

**Ideas not tried yet:**
- "Assisted temporal": the game's temporal accumulation only on young pixels, like the spatial assist.
- Measure whether the diffuse hit distance guide (`DLSSD.DiffuseHitDistance`, already implemented,
  on by default, **never measured**) helps RR in motion.
- `nvngx_dlssd.dll` also accepts `DiffuseRayDirection`, `DisocclusionMask` and `ResponsivityMask`
  (strings found in the DLL). A disocclusion mask from the game's history count might help RR.

## 2. Mirror-like reflections stay noisy

**Seen:** glass that reflects like a mirror and glossy metallic bodywork (a bright red car) show raw
noise with RR, especially in Full RR.

**Hypothesis:** RR gets no **specular motion vectors** and no **specular hit distance**
(`kBufferTypeSpecularMotionVectors` = 10, `kBufferTypeSpecularHitDistance` = 42), so it reprojects the
reflection with the motion of the surface, rejects its history and leaves it raw. Glass is not in the
RT G-buffer at all. Speedlemur's Control RR added specular motion vectors computed from the hit
distance (virtual point = surface + view direction × hit distance).

**Difficulty:** Forza's reflections are inline ray queries (`SSR_FullScreen_*`, hybrid screen space +
DXR, checkerboard) plus a mip pyramid chosen by roughness ([FRAME-MAP.md](FRAME-MAP.md)). The game
does **not store** a reflection hit distance: the SSR trace (`SSR_FullScreen_RayTrace_..._ContactHardened`,
`0x087EDF0D`) writes the reflection colour + confidence and an RGBA8_SNORM texture with the chosen mip
(log₂ of the cone footprint), the gloss and the confidence; the DXR pass writes the same two textures
(its pipeline is created before the add-on loads, so it has no hash and was never dumped).

**In progress:** "Specular motion vectors (experimental)" (Guides tab) computes them with a fixed,
user-set distance scaled by gloss², treating the reflection as static in the world (exact for glass and
puddles, and for car panels moving within their own plane). Not measured yet. Next steps if it helps:
a screen-space trace of the reflected ray against the depth for a real per-pixel distance, or patching
the SSR trace to export the distance it already computes.

## 3. Fireflies at night (Full RR)

**Seen:** isolated orange or white dots on walls lit indirectly at night.

**Measured and fixed in part:** the cause is the game's 3×3 resolve, which spreads a single bright
raw sample into a blotch before anything else (so clamps after the filter could not tell it apart from
real light). The "isolated sample clamp" acts before that resolve and removes most dots (k = 4 chosen).

**Still open:**
- The no-history path of the temporal filter (disocclusion) only uses the centre sample and is not
  clamped.
- Brightness by day with k = 4 is not confirmed (the bench run used for that had the wrong GI mode).

## 4. Tree tops burnt against the sky

**Seen:** the edges of tree tops against a bright sky look burnt (too bright) with RR.

**Measured:** +0.2 to +1.7 EV on edges next to the sky and 2–10× more almost-white pixels, in 5 scenes.
Exposure for RR, sky guide modes, foliage options, metalness and specular fade did **not** change it.

**Hypothesis:** foliage animated by wind has no motion vectors of its own, so RR blends old sky into
the leaves. Planned (not started): a `kBufferTypeBiasCurrentColorHint` mask (29; `nvngx_dlssd.dll`
mentions `DLSSD.ResponsivityMask`) only on foliage next to the sky, as an option. First, a diagnostic
"RR without history" to confirm.

## 5. Slow boiling with the game's original GI

**Seen:** slow blotches on night facades and in tunnel shadows with "Game original".

**Measured:** RR reduces it by ~17–22 %. History x2 / x4 do not improve it. Probably RT shadows rather
than GI. Not investigated further.

## Also wanted

- Tests on other RTX GPUs (20, 30, 50 series), other resolutions, DLAA and other DLSS modes. Open an
  issue with your results, even if everything works.
- Side-by-side screenshots or short videos (same spot, F11 toggles RR) for the README.
- A check of the game version you play: the GI modes depend on two shaders staying the same
  (SHA-256 in [game-shaders/README.md](../game-shaders/README.md)).
