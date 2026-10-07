# Third-party notices

rr-forza is released under the [MIT License](LICENSE). It includes or uses the following third-party
software. Their license texts are next to their files in `third_party/`.

| Component | Version | Where | License | Used for |
|---|---|---|---|---|
| [ReShade](https://github.com/crosire/reshade) add-on headers | 6.8.0 | `third_party/reshade/include` | BSD 3-Clause or MIT | Add-on API (events, overlay, config) |
| [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) headers | 2.14.1 | `third_party/streamline/include` | MIT | Types and constants of the Streamline API |
| [Dear ImGui](https://github.com/ocornut/imgui) header | 1.92.5 (docking) | `third_party/imgui` | MIT | Overlay UI (through ReShade's ImGui function table) |
| [MinHook](https://github.com/TsudaKageyu/minhook) | 1.3.4 | `third_party/minhook` | BSD 2-Clause | Function hooks (compiled into the add-on) |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | 1.9.2609 | downloaded to `third_party/dxc` by `tools/fetch-dxc.ps1` (not in the repository) | University of Illinois/NCSA + MIT | Build time only: compiles the HLSL shaders, disassembles, reassembles and signs the patched game shaders. Not shipped in the add-on. |

## Techniques from other projects

- **Control Ray Reconstruction** by speedlemur ([speedlemur/renodx](https://github.com/speedlemur/renodx),
  branch `control-rr`, MIT): answering the game's DLSS-SR evaluation with DLSS-RR, and replacing the
  game's denoiser shaders at runtime by matching their hash.
- **RenoDX** ([clshortfuse/renodx](https://github.com/clshortfuse/renodx), MIT): CRC32 of the shader
  bytecode as the shader identifier.

`shaders/specmv.hlsl` (specular motion vectors) is adapted from `specmv.cs_5_0.hlsl` of Control Ray
Reconstruction and keeps its notice:

```
Copyright (C) 2026 speedlemur
SPDX-License-Identifier: MIT
```

The MIT license text is the same as in [LICENSE](LICENSE). Apart from that file only the ideas are
reused; if you find something else that should carry another project's notice, please open an issue.

## Game and NVIDIA files

The repository contains no files from Forza Horizon 6 and no NVIDIA binaries. At runtime the add-on
uses the Streamline and DLSS libraries that the game itself installs. To build the GI modes, every
contributor exports two of the game's shaders from their own copy of the game
([game-shaders/README.md](game-shaders/README.md)); the release build contains modified versions of
those two shaders.
