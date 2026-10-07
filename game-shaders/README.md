# game-shaders/

The RTGI filter modes of the add-on (everything in the **Game GI** tab except "Game original") are
built by patching two of Forza Horizon 6's own compute shaders. Those shaders are the game's code,
so they are **not in this repository**: every contributor exports them from their own copy of the
game and puts them here.

| File | Shader | Size | SHA-256 (FH6 6.440.853.0) |
|---|---|---|---|
| `shader_0x4DAF8A48.cs.cso` | `RTGI_TemporalFilter` | 20 588 bytes | `24d16b420d900d0eb3d2f713e3c674de998bd4963321b032dece96fc66984f30` |
| `shader_0x209AB6A4.cs.cso` | `RTGI_SpatialFilter_Disk` | 6 724 bytes | `bbcf3ee1f9cd175d15782bd7e7e3b25b58ea5228614997a44f4e7f4655a7685c` |

The number in the name is the CRC32 of the DXIL container, the same hash RenoDX uses.

## How to export them

1. Install a build of the add-on (the official release is fine, or your own build without these
   files: it still loads, only the GI variants are missing).
2. Start the game and drive for a few seconds with ray traced global illumination enabled.
3. Open the ReShade overlay (`Home` by default), go to the **Ray Reconstruction** tab, then
   **Developer** > **Export game shaders**. The status line must say `2/2 exported to ...`.
4. Copy both `.cso` files from `<game folder>\rr-forza-captures\game-shaders\` into this folder.
5. Rebuild (`bash build.sh`). CMake disassembles them with `dxc -dumpbin` and
   `scripts/patch_rtgi*.pl` patches the disassembly.

If CMake warns that the SHA-256 differs, your game version is not the one the patches were written
for. The build then stops at the first anchor that no longer matches
(`anchor found N times (expected 1)`). See "When the game updates" in
[CONTRIBUTING.md](../CONTRIBUTING.md).

Everything else in this folder is ignored by git, so you can keep your disassembly listings
(`.ll`) or other dumps here.
