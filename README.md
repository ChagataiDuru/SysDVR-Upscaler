# SysDVR-Upscaler

SysDVR-Upscaler is a Windows-first C++20/Vulkan client for low-latency Nintendo Switch SysDVR video. It accepts recorded H.264 captures or a live SysDVR bridge stream, performs explicit YUV color reconstruction, and compares spatial upscalers before presentation.

The current implementation includes software decode, optional FFmpeg D3D11VA decode, direct CPU-NV12 upload into Vulkan, a validated GPU-resident D3D11/Vulkan interop-copy path, eight upscaling modes, split comparison, timing telemetry, and screenshots. Strict imported-NV12 zero-copy is intentionally unavailable on the tested NVIDIA driver.

## Milestone status

| Milestone | Status |
| --- | --- |
| Phase 1: offline H.264/Vulkan playback | Implemented and validated with recorded samples |
| Phase 1.5: presentation, telemetry, chroma, and quality hardening | Implemented |
| Phase 2: live SysDVR bridge and latest-frame queue | Implemented; Switch hardware validation remains pending |
| Phase 3.0: decoder and interop capability reporting | Implemented |
| Phase 3.1: D3D11VA decode with CPU readback | Implemented |
| Phase 3.2: native CPU-NV12 Vulkan upload | Implemented; manual hardware comparison remains pending |
| Phase 3.3: D3D11/Vulkan NV12 interop | Closed with `interop-copy`: pixel-exact on all three recorded samples and faster than readback; strict imported-NV12 zero-copy is unsupported on NVIDIA 616.92; Switch hardware acceptance pending |

Do not treat the Phase 2 or Phase 3 status as a completed hardware-success claim. USB disconnect/reconnect, visual comparison, and a 30-minute live soak still need to be run on Switch hardware.

## Windows prerequisites

- Windows 10/11 x64 and a Vulkan 1.2-capable GPU/driver.
- Visual Studio 2022 C++ tools, CMake 3.24+, and Ninja.
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/) with `VULKAN_SDK` set. Its `glslc` is preferred; `glslangValidator` is accepted.
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. The manifest supplies GLFW, Dear ImGui, FFmpeg development libraries, stb, and doctest.

The FFmpeg command-line download alone is insufficient: compilation needs the `libavformat`, `libavcodec`, and `libavutil` headers and import libraries. See [Windows dependencies](docs/dependencies-windows.md).

## Configure, build, and test

```powershell
git clone https://github.com/microsoft/vcpkg $env:USERPROFILE\src\vcpkg
& $env:USERPROFILE\src\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$env:USERPROFILE\src\vcpkg"
$env:VULKAN_SDK = 'C:\VulkanSDK\<version>'

cmake --preset win-release
cmake --build --preset win-release
```

Debug enables Vulkan validation by default. Dependency-light tests use:

```powershell
cmake --preset core-tests
cmake --build --preset core-tests
ctest --preset core-tests
```

On a Windows GPU host, enable the opt-in recorded-file interop-copy tests with `-DNS60_ENABLE_GPU_TESTS=ON`; they compare `flat_color`, `ui_text`, and `fast_motion` against readback with validation enabled.

Build the managed SysDVR bridge separately when using live input:

```powershell
.\scripts\build-sysdvr-upscaler-bridge.ps1
```

## Decoder diagnostics

These commands do not require an input file:

```powershell
.\build\win-release\NexusStream60.exe --list-decoders
.\build\win-release\NexusStream60.exe --decoder-capabilities
```

The capability report separately shows extension availability, NV12/D3D11-fence feature support, and whether a real decoder texture has been imported. The standalone report cannot perform the last check without an H.264 stream.

## Recorded-file playback

```powershell
.\build\win-release\NexusStream60.exe `
  --input '.\samples\ui_text_720p60.mp4' `
  --width 1920 --height 1080 `
  --upscale fsr1-easu-rcas --rcas-sharpness 0.25 --loop
```

Expected input is even-sized 8-bit H.264 `yuv420p`/`yuvj420p` with supported range and matrix metadata and left-sited chroma. The verified baseline samples are 1280×720, limited-range BT.709 at approximately 59.8–59.9 FPS. File timing uses decoded presentation timestamps rather than assuming 60 Hz.

## Live SysDVR playback

For a unified launch, let NexusStream60 start the bridge and create a unique pipe:

```powershell
.\build\win-release\NexusStream60.exe `
  --source sysdvr `
  --sysdvr-bridge '.\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe' `
  --decoder d3d11va --decoder-path interop-copy `
  --quality-preset balanced --presentation exact `
  --latency-profile balanced --live-frame-queue-depth 1 `
  --fullscreen --borderless
```

For a manual two-process launch, start NexusStream60 first:

```powershell
.\build\win-release\NexusStream60.exe `
  --source sysdvr-pipe --pipe-name SysDVR-Upscaler.Video `
  --quality-preset balanced --presentation exact `
  --latency-profile balanced --live-frame-queue-depth 1 `
  --decoder d3d11va --decoder-path readback
```

Then start the bridge:

```powershell
.\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe usb `
  --upscaler-video-pipe SysDVR-Upscaler.Video `
  --upscaler-pipe-queue-messages 16 `
  --upscaler-pipe-queue-bytes 1048576 `
  --upscaler-pipe-max-age-ms 50 --no-audio
```

Decoder selection:

- `--decoder software` is the default software H.264 path.
- `--decoder d3d11va --decoder-path readback` uses hardware decode, CPU NV12 readback, and the Phase 3.2 native NV12 Vulkan upload.
- `--decoder auto` tries D3D11VA and falls back to software; it continues to use readback.
- `--decoder d3d11va --decoder-path interop` is reserved for strict imported-NV12 zero-copy and exits with an explanatory unsupported error on the tested NVIDIA path; it never falls back silently.
- `--decoder d3d11va --decoder-path interop-copy` is the recommended Windows D3D11VA fast path. Frames remain GPU-resident; one D3D11 copy and a plane-split compute pass produce shared R8/R8G8 textures for Vulkan. The portable global default remains `readback`.

## Quality modes and controls

Available modes are `nearest`, `bilinear`, `bicubic`, `lanczos2`, `bilinear-cas`, `lanczos2-cas`, `fsr1-easu`, and `fsr1-easu-rcas`. EASU/RCAS and standalone CAS use the pinned official AMD FidelityFX FSR1/CAS source under `third_party/fidelityfx-fsr1/`.

Controls: `Space` pause/resume, `Right` step while paused, `Home` restart, `F11` fullscreen, `Tab` telemetry, `S` screenshot, and `Escape` leave fullscreen/exit. Number keys `1`–`8` select upscalers; `C` toggles comparison; `A`/`B` assign its sides; drag with the left mouse button to move the divider; `[`/`]` adjust sharpness; `Z` toggles the zoom inspector.

Captures and adjacent JSON metadata are written under `captures/`. Use borderless fullscreen for exact 1920×1080 presentation on a 1080p display.

## Documentation

- [Phase 1 architecture](docs/phase1-architecture.md)
- [Phase 1.5 quality hardening](docs/phase1-5-quality-hardening.md)
- [Phase 2 live bridge](docs/phase2-live-bridge.md)
- [Phase 3 software baseline](docs/phase3/software-baseline.md)
- [Phase 3.1 D3D11VA readback](docs/phase3/d3d11va-readback.md)
- [Phase 3.2 native NV12 upload](docs/phase3/nv12-vulkan-path.md)
- [Phase 3.3 D3D11/Vulkan interop](docs/phase3/d3d11-vulkan-interop.md)
- [Upscaler behavior](docs/upscalers.md), [presentation mapping](docs/presentation-mapping.md), and [telemetry methodology](docs/telemetry-methodology.md)

Direct Vulkan Video H.264 decode is the next true-zero-copy candidate. It requires rebuilding FFmpeg with Vulkan support and is deliberately a separate phase. NVIDIA Image Scaling, compression preprocessing, Nexus Adaptive Detail, presets, repeatable multi-mode capture, audio, and networking also remain future work.

## Troubleshooting

- **No GLSL-to-SPIR-V compiler:** set `VULKAN_SDK` and confirm its `Bin` directory contains `glslc.exe` or `glslangValidator.exe`.
- **FFmpeg development files were not found:** use the manifest toolchain or point `FFMPEG_ROOT` at a development tree containing `include` and `lib` directories.
- **Missing validation layer:** install the Vulkan SDK or pass `--validation off`; Release defaults to validation off.
- **Unsupported color metadata:** inspect recorded inputs with `scripts/validate-samples.ps1`.
- **Interop selected but unavailable:** strict `interop` is intentionally unsupported on the tested NVIDIA driver. Use `--decoder-path interop-copy`; run `--decoder-capabilities` to inspect same-GPU LUID, Win32 external-memory, timeline semaphore, and D3D11-fence import support.
- **Black output or validation errors:** update the GPU driver, run Debug with validation enabled, and capture the selected GPU, decoder format, and frame-storage telemetry.
