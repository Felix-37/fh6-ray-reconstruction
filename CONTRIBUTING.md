# Contributing

Thank you for helping. I started this project alone as an experiment, and I got stuck on problems
that need more knowledge than I have. Any help counts: testing, measuring, screenshots, ideas backed
by evidence, or code.

English is preferred in issues and pull requests so that everyone can follow, but Spanish is fine too
(I speak Spanish).

## Ways to help

- **Test it** on your RTX GPU and open an issue with the result, even if everything works (GPU,
  driver, game version, DLSS mode, resolution, what you saw).
- **Share comparisons:** screenshots or short videos at the same spot with RR on and off (F11).
- **Work on an open problem:** [docs/OPEN-PROBLEMS.md](docs/OPEN-PROBLEMS.md). Comment on the issue
  (or open one) before starting, so that efforts do not overlap.
- **Translate:** the overlay has English and Spanish ([src/overlay.cpp](src/overlay.cpp), `rr::tr`), and
  the README has several languages ([i18n/](i18n/)). Corrections from native speakers are welcome.

## Before writing code

1. Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), especially
   [Rules learned the hard way](docs/ARCHITECTURE.md#rules-learned-the-hard-way). Breaking one of them
   usually means a GPU hang or a corrupted image.
2. Read [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md), so you do not repeat a discarded idea.
3. Build it: [docs/BUILDING.md](docs/BUILDING.md).

## How changes are judged

Image quality claims need **measurements**, not only impressions:

- Use the bench (Measurement tab) before and after your change, in the same spot, with the camera
  still. Attach `report.txt` and look at `report.html`: the noise map and the crop often show what
  the numbers miss (passing cars, grain, smearing).
- Trust the control variant at the end of a run more than the first RR variant after DLSS-SR.
- For motion problems, use "Measure disocclusion" and confirm by driving.
- Defaults change only with measurements that support the change.

## Code conventions

- Match the surrounding code: C++20, 4-space indentation, `snake_case`, comments that explain *why*
  (dates of measurements and incidents are welcome in comments, they are the project's history).
- Every user-visible setting:
  - lives in `rr::cfg` (`src/settings.cpp`) and is saved in `ReShade.ini [RR_Forza]`;
  - shows in the overlay whether it really reached the GPU (`[OK]` / `[...]` / `[ERROR]`, the
    `feedback` helpers in `src/overlay.cpp`), never just "clicked";
  - has its text in English and Spanish (`rr::tr("English", "Español")`). If you do not speak
    Spanish, write the English text and copy it into the Spanish slot; someone will translate it.
- Log what the add-on detects and decides (`rr::log_info` / `rr::log_warn`), once per change, not
  every frame.
- Never touch the game executable or its files on disk. Hooks go into Streamline exports or the D3D12
  runtime only.
- The README (all languages) and the docs are written in the first person, by me as the author ("I
  measured", "my recommendation"), never in the third person ("the author"). Keep that voice when you
  edit them, and update the translations in `i18n/` when you change the English README.
- Never commit game files (shaders, dumps, captures). `game-shaders/`, `captures/` and
  `rr-forza-captures/` are ignored for that reason.

## When the game updates

A game update can change:

- **The RTGI shaders.** The GI tab then says "not seen", or the build warns that the SHA-256 of
  `game-shaders/` differs and stops at `anchor found N times (expected 1)`. Export the new shaders,
  diff the new disassembly (`build/generated/rtgi_*.orig.ll`) against the old one, update the anchors
  and register numbers in `scripts/patch_rtgi*.pl`, then the CRCs in `src/replace.cpp`
  (`kTargets`, `kUpscaleCrc`), `src/shader_dump.cpp` (`export_game_shaders`) and `CMakeLists.txt`.
- **Streamline.** `slInit` is patched by hand: `src/slinit_hook.cpp` expects the prologue of
  Streamline 2.14.1 and logs `unexpected prologue` otherwise.
- **The RT G-buffer.** The guides find it by format and pass. If RR falls back with "guide buffers not
  built yet", capture a frame (F10) and compare with [docs/FRAME-MAP.md](docs/FRAME-MAP.md).

## Pull requests

- One topic per pull request, with a short description of what changed and why.
- Say how you tested it: game version, GPU, DLSS mode, bench results (before and after) when the
  image changes.
- It must build with `bash build.sh` without new warnings in our own code.
- By contributing, you agree that your contribution is licensed under the MIT License.
