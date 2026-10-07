# Building

The add-on is built with free tools only, on Windows 10/11.

## Tools

| Tool | Version used | Notes |
|---|---|---|
| [Git for Windows](https://git-scm.com/download/win) | 2.5x | Provides Git Bash and Perl (used by the shader patch scripts). |
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) | UCRT x86_64 | Clang + MinGW-w64. MSVC is not supported: the code uses MSVC-style SEH (`__try`), which Clang accepts and GCC does not. |
| [CMake](https://cmake.org/download/) | 3.20 or newer | |
| [Ninja](https://github.com/ninja-build/ninja/releases) | any | |
| DirectX Shader Compiler | 1.9.2609 | Downloaded by `tools/fetch-dxc.ps1` (checked by SHA-256). |

Portable (zip) versions of llvm-mingw, CMake and Ninja are fine. Either put their `bin` folders in
`PATH`, or create `build.local.sh` next to `build.sh` (ignored by git):

```bash
LLVM_MINGW=/c/tools/llvm-mingw
CMAKE=/c/tools/cmake/bin/cmake.exe
NINJA=/c/tools/ninja/ninja.exe
```

## Steps

```bash
git clone https://github.com/Felix-37/fh6-ray-reconstruction.git
cd fh6-ray-reconstruction
powershell -ExecutionPolicy Bypass -File tools/fetch-dxc.ps1   # once
bash build.sh
```

The result is `build/rr-forza.addon64` (and `build/rr_metrics.exe`).

### Game shaders

Without the two game shaders in `game-shaders/`, CMake prints a warning and builds the add-on
**without the RTGI filter variants**: RR, the guides, sharpening, the bench and the capture work, and
only the "Game original" GI mode is available. To build the full add-on, export the shaders from your
own copy of the game (with any build of the add-on, including the one you just made) and rebuild:
[game-shaders/README.md](../game-shaders/README.md).

## Installing a development build

Copy `build/rr-forza.addon64` into the game folder (next to `ForzaHorizon6.exe`). The game must be
closed: Windows locks the file while it runs. Keep a copy of the previous version to compare.

## Debugging

- **Log:** `ReShade.log` in the game folder. The add-on writes what it detects and decides (lines
  starting with `[RR Forza]`): the `slInit` feature list, the first RR evaluation, every fallback
  reason, the GI pipelines it found, the variants it created.
- **Overlay:** every setting shows whether it really reached the GPU. The status table at the top
  shows RR's state, the last result and the fallback reason.
- **Crash dumps:** Windows writes them to `%LOCALAPPDATA%\CrashDumps\forzahorizon6.exe.*.dmp` when
  local dumps are enabled. To map an address inside the add-on to a line, build an unstripped copy
  (remove `-s` from the Release link options) and use `llvm-addr2line` from llvm-mingw.
- **Frame capture:** Developer tab (or F10 with developer hotkeys). The files go to
  `<game folder>\rr-forza-captures\` unless `CaptureDir` is set in `ReShade.ini [RR_Forza]`.
- **Bench re-analysis:** `build/rr_metrics.exe [--en|--es] <bench folder>` rewrites the report from the
  saved frames, for example after changing `src/metrics.cpp`.

## Line endings and encodings

`.gitattributes` keeps shell and Perl scripts in LF and PowerShell scripts in CRLF. Source files are
UTF-8. Spanish strings in the overlay may use `\xC3\xB3`-style escapes or plain UTF-8; both work.
