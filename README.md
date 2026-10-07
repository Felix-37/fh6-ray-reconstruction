<div align="center">

# Ray Reconstruction for Forza Horizon 6

**DLSS Ray Reconstruction (DLSS-RR) in Forza Horizon 6 on PC, as a ReShade add-on.**<br>
A better denoiser for the game's ray traced lighting, for every NVIDIA RTX graphics card.

**English** ·
[Español](i18n/README.es.md) ·
[Português (BR)](i18n/README.pt-BR.md) ·
[Français](i18n/README.fr.md) ·
[Deutsch](i18n/README.de.md) ·
[Italiano](i18n/README.it.md) ·
[Polski](i18n/README.pl.md) ·
[Русский](i18n/README.ru.md) ·
[Türkçe](i18n/README.tr.md) ·
[日本語](i18n/README.ja.md) ·
[한국어](i18n/README.ko.md) ·
[简体中文](i18n/README.zh-CN.md)

![Status: beta](https://img.shields.io/badge/status-beta-orange)
![Forza Horizon 6](https://img.shields.io/badge/Forza%20Horizon%206-6.440.853.0-0a7cff)
![NVIDIA RTX](https://img.shields.io/badge/GPU-NVIDIA%20RTX-76b900)
![ReShade 6.8+](https://img.shields.io/badge/ReShade-6.8%2B%20add--on-8a2be2)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

</div>

> [!IMPORTANT]
> Beta. I made it alone and tested it only on my PC. It works, but some problems are still open and I am
> stuck on them. If you know graphics programming, DLSS/Streamline or shader modding, your help is very
> welcome: see [where help is needed](#where-help-is-needed).

## What it does

Forza Horizon 6 offers DLSS Super Resolution, DLAA, Frame Generation and Multi Frame Generation, but
not Ray Reconstruction, even though the game folder already contains the Streamline RR plugin
(`sl.dlss_d.dll`) and the RR model (`nvngx_dlssd.dll`). This add-on turns it on:

1. It adds DLSS-RR to the features the game asks Streamline to load.
2. Every time the game evaluates DLSS Super Resolution, it evaluates DLSS Ray Reconstruction instead,
   with the same inputs. Any frame where something is missing falls back to the game's own DLSS.
3. Every frame, it builds the guide buffers that RR needs from the game's ray tracing G-buffer:
   world normals + roughness, diffuse albedo, specular albedo and diffuse hit distance.
4. It can change the game's own ray traced GI denoising filters: by default they accumulate four
   times more history ("History x4", the most stable image), and in "Full RR" mode they are removed so
   that RR denoises the raw global illumination itself.

The game's executable and files are never modified. Everything happens in memory, inside ReShade.

**What you get:** cleaner ray traced lighting, with less "boiling" and flicker in the indirect light
(walls at night, tunnels, shadows). Thin geometry such as fences and cables is often sharper.

**What it costs:** some GPU time and VRAM. See [Performance](#performance).

## Comparison

I recorded these clips in the game on my PC (RTX 4050 Laptop GPU, 1920×1200, car standing still).
They show real pixels at 1:1, cropped from the recording. The GI mode was **"Full RR"**
(experimental), not the default "History x4".

**Tunnel, cockpit view.** I press F11 and RR turns off: the gloves and the steering wheel start to boil.

<p align="center"><img src="docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="Cockpit in a tunnel: RR on, then F11 turns it off and the gloves start to boil"></p>

**Same tunnel, side by side** (left: off, right: on).

<p align="center"><img src="docs/images/boiling-cockpit-tunnel.gif" width="800" alt="Steering wheel in a tunnel, RR off on the left and on on the right"></p>

**Cockpit on a forest road.** Two moments of the same shot, 4 s apart, in a loop: off, then on. With
RR the gloves, the A-pillar and the dashboard stop boiling, and the Audi rings on the wheel get their
metallic reflection back.

<p align="center"><img src="docs/images/boiling-cockpit-forest.gif" width="800" alt="Cockpit on a forest road, RR off and then on"></p>

In the forest clip, the temporal noise of the right glove drops from 2.3 to 0.3–0.5 (measured on the
recording). With RR the cockpit also looks a little darker, and I do not know yet which of the two is
closer to the right brightness. Outside the car, by day, the difference is much smaller. Details in
[docs/MEASUREMENTS.md](docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026).

## Requirements

- An **NVIDIA GeForce RTX** graphics card. Ray Reconstruction runs on every RTX generation (20, 30,
  40 and 50 series), but so far I have only tested it on my RTX 4050 Laptop GPU (6 GB).
- **Forza Horizon 6** for PC, version **6.440.853.0** (the version it was developed on). On other
  versions, RR itself should still work. The GI modes need the game's shaders to be unchanged, and
  the menu tells you when they are not.
- In the game's video settings: **DLSS** as the upscaler (any quality mode) and **ray traced global
  illumination** on. Frame Generation and Multi Frame Generation can stay on.
- **ReShade 6.8.0 or newer with full add-on support** (from [reshade.me](https://reshade.me), the
  download "with full add-on support").
- The game's own Streamline RR files (`sl.dlss_d.dll`, `nvngx_dlssd.dll`). They come with the game.
  Tested with Streamline 2.14.1 and DLSS-RR 310.9.0 / 310.9.1.

## Installation

1. Install **ReShade with full add-on support** for `ForzaHorizon6.exe` and select **DirectX 10/11/12**.
   You do not need any effect shaders.
2. Download `rr-forza-<version>.zip` from [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases) and copy `rr-forza.addon64`
   into the game folder, next to `ForzaHorizon6.exe`.
3. Start the game. In the video settings, select DLSS and turn ray traced global illumination on.
4. Drive for a few seconds, then open the ReShade overlay (`Home` key by default). In the
   **Ray Reconstruction** tab, the status must say **"Ray Reconstruction active"**.

**To uninstall,** delete `rr-forza.addon64`. Its settings are in `ReShade.ini`, section `[RR_Forza]`,
and you can delete that section too.

## Usage

- **F11** switches between Ray Reconstruction and the game's normal DLSS, to compare.
- The **Ray Reconstruction** tab of the ReShade overlay has every setting. Each setting shows whether
  it really reached the GPU (`[OK]`, `[...]` or `[ERROR]`).
  - **General:** language (English / Español), RR on/off, RR preset (F recommended), RR sharpening.
  - **Guides:** how surfaces are described to RR (foliage handling, metalness), plus a diagnostic.
  - **Game GI:** what happens to the game's ray traced GI filters (see the table below).
  - **Measurement:** a built-in bench that compares RR with normal DLSS using real numbers.
  - **Developer:** frame captures, game shader export and developer hotkeys (F7, F10; off by default).
  - **Help:** keys, files and known limits.

| Game GI mode | What it does |
|---|---|
| Game original | The game's RTGI filters stay untouched, and RR cleans their output. |
| History x2 | The game's temporal filter accumulates 2 times more frames: less boiling, but slower light response. |
| **History x4** (default, recommended) | The same with 4 times more frames: the most stable image, with good quality and none of the open problems of Full RR. The indirect light reacts a little more slowly (possible trails on moving shadows). |
| No temporal filter | Raw GI of every frame goes to RR. Experimental. |
| Full RR | The game's temporal and spatial RTGI filters are replaced, so RR does all the denoising. The best image by day (no boiling of the indirect light at all), but with the open problems listed under [Known issues](#known-issues). While the camera moves, it hands over to "History x4" automatically. Experimental. |

The defaults are my recommendation, chosen after the measurements in
[docs/MEASUREMENTS.md](docs/MEASUREMENTS.md) and many hours of driving. "Restore defaults" (Guides tab)
brings them back.

## Performance

Measured on my RTX 4050 Laptop GPU, at 1920×1200 output, 1280×800 internal, with Multi Frame
Generation 4x and a 138 fps cap:

- RR costs about 0.9 ms of GPU time per presented frame more than DLSS-SR in the same quality mode
  (−6 % fps when the GPU is the limit).
- RR at DLSS **Balanced** cost about the same as the game's DLSS-SR at **Quality** (same fps, same
  GPU load), with no visible loss. This is the combination I recommend.
- Both models stay loaded so that F11 switches cleanly. That uses a few hundred MB of extra VRAM,
  which is tight on 6 GB cards: the game already reports "out of VRAM" warnings there without RR.

## Known issues

These are open, with the evidence collected so far in [docs/OPEN-PROBLEMS.md](docs/OPEN-PROBLEMS.md):

1. **Noise in motion (Full RR):** for a few frames, raw noise shows when you move or when something
   comes back on screen. "GI history x4 while moving" and "Disocclusion assist" reduce it, but it is
   not gone.
2. **Mirror-like reflections** (glass, chrome, glossy paint) stay noisy, above all with Full RR. RR
   probably needs specular motion vectors; the experimental option "Specular motion vectors" (Guides
   tab, off by default) provides them with an estimated distance, and is not measured yet.
3. **Fireflies at night (Full RR):** much less frequent with the "isolated sample clamp", but not zero.
4. **Tree tops against a bright sky** can look burnt (too bright) with RR.
5. Slow **boiling in tunnels** with the game's GI filters (original or History x4): RR reduces it
   (−17 to −22 %), but does not remove it; it may come from the RT shadows rather than the GI.

## FAQ

**Does it work on AMD or Intel GPUs?** No. DLSS Ray Reconstruction only runs on NVIDIA RTX GPUs.

**Can I get banned?** I do not know of any case, but I cannot promise anything. The add-on does not
touch the game's files. It hooks NVIDIA's Streamline library and Direct3D 12 inside the game process,
as ReShade add-ons do. Online use is at your own risk.

**Does it work with other mods?** I use it together with the RenoDX "MFG Unlock" add-on without
problems. Other combinations (OptiScaler, DLSS DLL swaps) are untested.

**The Game GI tab says the filters were "not seen" or a variant was "not created".** Drive for a few
seconds first. If it stays like that, your game version probably has different shaders. Please open
an issue with your game version and your `ReShade.log`.

**Where is the log?** `ReShade.log` in the game folder. The lines of this add-on start with `[RR Forza]`.

## Where help is needed

This project needs people who know more than I do about some of these areas:

- **Specular motion vectors and specular hit distance for RR**, to fix the noisy reflections. The
  speedlemur Control RR mod did this. Forza's reflections are inline ray queries plus a mip pyramid,
  described in [docs/FRAME-MAP.md](docs/FRAME-MAP.md).
- **Disocclusion noise in Full RR:** how to give RR a better start for pixels that have no history.
- **Burnt tree tops:** foliage animated by wind has no motion vectors of its own. Would a
  responsiveness or "current colour" mask help?
- **Testing on other RTX GPUs**, resolutions and game versions, and **side-by-side screenshots or
  videos** (same spot, F11 toggles RR). There are no comparison images in this README yet.

Start with [CONTRIBUTING.md](CONTRIBUTING.md), then [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
(how the add-on works) and [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md) (what was tried, measured
and discarded, so that nobody repeats it). Building it needs only free tools: [docs/BUILDING.md](docs/BUILDING.md).

## Credits

- **speedlemur**, for [Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr):
  the same idea (answering the game's DLSS-SR evaluation with DLSS-RR) and the runtime shader
  replacement technique.
- **crosire**, for [ReShade](https://github.com/crosire/reshade) and its add-on API.
- **clshortfuse and the RenoDX contributors**, for [RenoDX](https://github.com/clshortfuse/renodx),
  whose shader hashing convention is used here.
- **NVIDIA**, for [Streamline](https://github.com/NVIDIA-RTX/Streamline) and DLSS.
- **Tsuda Kageyu** ([MinHook](https://github.com/TsudaKageyu/minhook)), **Omar Cornut**
  ([Dear ImGui](https://github.com/ocornut/imgui)) and **Microsoft**
  ([DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)).

Third-party licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

This is an unofficial fan project. It is not affiliated with or endorsed by Microsoft, Xbox Game
Studios, Playground Games or NVIDIA. Forza Horizon is a trademark of Microsoft. NVIDIA, RTX and DLSS
are trademarks of NVIDIA Corporation.

The repository contains no game files. To provide the GI modes, the release build contains modified
versions of two of the game's compute shaders, which are useless without the game. Use at your own
risk.

The code is released under the [MIT License](LICENSE).
