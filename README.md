# NexusStream60

NexusStream60 is a Windows-first C++20/Vulkan laboratory for decoding recorded Nintendo Switch SysDVR MP4 video, performing explicit YUV color conversion, and comparing low-latency spatial upscalers. Phase 1 is an offline, timestamp-correct vertical slice: H.264 software decode, owned YUV420P frames, BT.709 conversion, bilinear 720p-to-1080p compute upscale, presentation, telemetry, and screenshots.

USB/SysDVR protocol ingestion, live timing reconstruction, audio, hardware decoding, networking, decoder/GPU zero-copy, SPS patching, and advanced/temporal upscalers are deliberately out of scope.

## Windows prerequisites

- Windows 10/11 x64 and a Vulkan 1.2-capable GPU/driver.
- Visual Studio 2022 C++ tools, CMake 3.24+, and Ninja.
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/) with `VULKAN_SDK` set. Its `glslc` is preferred; `glslangValidator` is accepted.
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. The manifest supplies GLFW, Dear ImGui, FFmpeg development libraries, stb, and doctest.

The FFmpeg command-line download alone is insufficient: compilation needs `libavformat`, `libavcodec`, and `libavutil` headers/import libraries. With no vcpkg toolchain, set `FFMPEG_ROOT` to a conventional `include`/`lib` development tree; CMake prints a precise diagnostic when it cannot find one. See [Windows dependencies](docs/dependencies-windows.md).

## Configure and build

```powershell
git clone https://github.com/microsoft/vcpkg $env:USERPROFILE\src\vcpkg
& $env:USERPROFILE\src\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$env:USERPROFILE\src\vcpkg"
$env:VULKAN_SDK = 'C:\VulkanSDK\<version>'

cmake --preset win-release
cmake --build --preset win-release
```

Debug uses validation by default; Release does not require validation layers. Core ownership/timing/math tests can be built separately with `cmake --preset core-tests`, `cmake --build --preset core-tests`, and `ctest --preset core-tests`.

## Run

```powershell
.\build\win-release\NexusStream60.exe `
  --input '.\samples\ui_text_720p60.mp4' `
  --width 1920 --height 1080 --upscale bilinear --loop
```

Paths with spaces are supported. Expected input is H.264 High Profile, even-sized 8-bit `yuv420p`/`yuvj420p`, explicit supported range/matrix metadata, and left-sited chroma. The verified baseline is 1280×720, limited-range BT.709 at approximately 59.8–59.9 FPS. Timing always comes from decoded presentation timestamps, not a 60 Hz assumption.

Controls: `Space` pause/resume, `Right` step while paused, `Home` restart, `F11` fullscreen, `Tab` telemetry, `S` screenshot, and `Escape` leave fullscreen/exit. Captures and adjacent JSON metadata are written under `captures/`.

Useful options include `--fullscreen`, `--borderless`, `--vsync on|off`, `--validation on|off`, `--drop-late-frames`, and `--log-level trace|debug|info|warning|error|critical`. Run `--help` for the full list.

## Troubleshooting

- “No GLSL-to-SPIR-V compiler”: set `VULKAN_SDK` and ensure its `Bin` directory contains `glslc.exe` or `glslangValidator.exe`.
- “FFmpeg development files were not found”: use the manifest toolchain or point `FFMPEG_ROOT` at development headers and import libraries. `ffmpeg.exe` does not contain them.
- Missing validation layer: install the SDK or pass `--validation off`; Release defaults off.
- Unsupported/unspecified color metadata: inspect the file with `scripts/validate-samples.ps1`. Phase 1 rejects ambiguity instead of guessing and producing subtly wrong color.
- Black output or validation errors: update the GPU driver, run Debug with validation, and include the selected-GPU/format startup log in reports.

Architecture and ownership details are in [Phase 1 architecture](docs/phase1-architecture.md); the sample test matrix is in [Phase 1 validation](docs/phase1-validation.md).

## Spatial quality milestone

Eight live modes are available: `nearest`, `bilinear`, `bicubic`, `lanczos2`, `bilinear-cas`, `lanczos2-cas`, `fsr1-easu`, and `fsr1-easu-rcas`. EASU/RCAS use the pinned official AMD FidelityFX FSR1 source; standalone CAS uses the official FidelityFX CAS source. Copyright and MIT notices are preserved under `third_party/fidelityfx-fsr1/`.

```powershell
.\build\win-release\NexusStream60.exe `
  --input '.\samples\flat_color_720p60.mp4' --width 1920 --height 1080 `
  --upscale fsr1-easu-rcas --rcas-sharpness 0.25 --fullscreen --borderless --loop

.\build\win-release\NexusStream60.exe `
  --input '.\samples\ui_text_720p60.mp4' --compare bilinear,fsr1-easu-rcas `
  --rcas-sharpness 0.25 --fullscreen --borderless --loop
```

Live controls: number keys 1?8 select modes; `C` toggles split comparison; `A`/`B` assign the current mode to either side; drag the left mouse button to move the divider; `[`/`]` adjust active sharpness (hold Shift for larger steps); `Z` pauses and toggles the nearest-sampled zoom inspector. ImGui exposes exact CAS/RCAS values and anti-ringing.

`--width`/`--height` request reconstruction and client framebuffer size. A decorated window may be constrained by the Windows work area; telemetry explicitly reports the actual framebuffer, viewport, final resample, and 1:1 status. Use borderless fullscreen for true 1920x1080 on a 1080p monitor. Exact-size presentation uses texel fetches and does not add another linear filter.

Start conservatively at CAS 0.35 or RCAS 0.25. Spatial upscaling improves edge reconstruction and perceived readability, but a 720p source does not become true native 1080p detail. See [upscaler behavior](docs/upscalers.md), [presentation mapping](docs/presentation-mapping.md), and the [validation record](docs/upscale-comparison.md).

## Phase 1.5 hardening status

This workspace includes a Phase 1.5 hardening pass for exact presentation geometry, corrected active-playback telemetry, and selectable YUV420 chroma reconstruction. New options include `--monitor`, `--final-filter`, and `--chroma-upscale`. See [Phase 1.5 quality hardening](docs/phase1-5-quality-hardening.md), [telemetry methodology](docs/telemetry-methodology.md), and [chroma reconstruction](docs/chroma-reconstruction.md).

NVIDIA Image Scaling, compression preprocessing, Nexus Adaptive Detail, presets, and repeatable multi-mode capture are documented as planned integration points and are not claimed as complete runtime modes in this pass.
