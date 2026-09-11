# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

SysDVR-Upscaler (executable/CMake project name `NexusStream60`, namespace `ns60`, option/macro prefix `NS60_`) is a Windows-first C++20/Vulkan 1.2 client that decodes Nintendo Switch SysDVR H.264 video (recorded files or a live bridge stream), does explicit YUV→RGB color reconstruction, runs one of eight spatial upscalers, and presents with telemetry. See `README.md` for the full CLI and keyboard controls; phase design docs live in `docs/` and `docs/phase3/`.

## Build and test

Requires `VCPKG_ROOT` (manifest in `vcpkg.json` supplies GLFW, ImGui, FFmpeg dev libs, stb, doctest) and `VULKAN_SDK` (for `glslc`/`glslangValidator`). Presets use Ninja; run from a VS 2022 x64 developer shell.

```powershell
cmake --preset win-release; cmake --build --preset win-release   # or win-debug (Vulkan validation on by default)
cmake --preset core-tests;  cmake --build --preset core-tests; ctest --preset core-tests
```

- `core-tests` sets `NS60_BUILD_APP=OFF`: it builds only `ns60_core` + doctest, with no Vulkan/FFmpeg/GLFW needed.
- Single test: doctest cases are registered individually via `doctest_discover_tests`, so `ctest --preset core-tests -R "<test case name>"`, or run `build\core-tests\tests\ns60_tests.exe -tc="<name>"` directly.
- Opt-in GPU interop-copy tests need a window-capable GPU host. With `NS60_ENABLE_GPU_TESTS=ON`, CTest registers available `flat_color`, `ui_text`, and `fast_motion` samples; run `ctest --test-dir build/win-debug -R d3d11_vulkan_interop_copy --output-on-failure`. They print `NS60_GPU_SKIP:` when a required capability is absent.
- Diagnostics without input: `NexusStream60.exe --list-decoders`, `--decoder-capabilities`.
- `scripts/run-samples.ps1` / `scripts/validate-samples.ps1` drive the recorded samples in `samples/` (gitignored, local only).
- Shaders in `shaders/` compile to SPIR-V at build time (`cmake/CompileShaders.cmake`) into `<build>/shaders`. The exe loads them from the absolute `NS60_SHADER_DIR` baked in at compile time, so any new shader must be added to the `ns60_compile_shaders(...)` list in `CMakeLists.txt`.
- Warnings: MSVC `/W4 /permissive-` via `ns60_enable_warnings`.

## Architecture

**Two targets.** `ns60_core` (static lib) holds the dependency-free logic: CLI parsing (`AppConfig`), `FramePool`/`FrameQueue`, the bridge wire protocol, `PlaybackClock`, upscaler/presentation math (`Upscaling`), metrics, and color math. `NexusStream60` adds everything touching FFmpeg, Vulkan, D3D11, GLFW, or ImGui. The unit tests link only `ns60_core`, so put new testable logic there.

**Flow** (`src/app/Application.cpp::runApplication`):
1. `main.cpp` parses the command line (`parseCommandLine` → `AppConfig`).
2. Source selection creates an `FFmpegVideoReader` from either a file path or `SysDvrPipeInput`. `--source sysdvr` also launches the managed bridge process (`CreateProcessW`).
3. A `std::jthread` decoder thread fills slots in a preallocated `FramePool` and publishes them through `FrameQueue`.
4. The main thread owns GLFW, all Vulkan objects, ImGui, the clock, and presentation. Per frame, `VideoPipeline::prepareFrame` → `recordProcessing` (color convert compute → upscale/sharpen compute passes) → `beginPresent`/`endPresent` (full-screen triangle into the swapchain).

**Frame ownership.** `FrameQueue` is a bounded SPSC queue of slot indices. The mutex covers only index/state transitions, never decode, copy, or render. It has two policies:
- **File playback** is PTS-timed. It preserves order (`acquireWrite`/`acquireReadPreserveOrder`), and full queues block rather than drop.
- **Live playback** is immediate, with latest-frame semantics (`acquireWriteLatest`/`tryAcquireNewest`). Stale frames are dropped and counted.

A separate display slot lets pause/repeat release queue ownership. Hot paths must not allocate. Pools, GPU resources, descriptors, and queries are created once (see `docs/phase1-architecture.md`).

**Frame storage variants** (`DecodedFrame.h`, `DecodedFrameStorage`):
- `CpuYuv420P`: software decode. Planar Y/U/V upload, `yuv420p_to_rgb.comp`.
- `CpuNv12`: D3D11VA with CPU readback. The `uPlane` field holds interleaved UV, and it uses `nv12_to_rgb.comp`.
- `D3D11Nv12`: D3D11/Vulkan interop. The frame carries a `shared_ptr<D3D11FrameLease>`. There are two decoder paths:
  - `--decoder-path interop` is reserved for strict imported-NV12 zero-copy and currently exits unsupported on the tested NVIDIA driver. It must never silently fall back.
  - `--decoder-path interop-copy`: `FFmpegVideoReader` copies each slice into a private NV12 texture, then a D3D11 compute shader splits it into a ring of shared R8 and R8G8 textures (`D3D11CopyRing`). The lease's description has a non-zero `chromaTextureIdentity`, and Vulkan imports two plain single-plane images. Vulkan waits on a shared D3D11 fence imported as a timeline semaphore. Each lease is held both by the in-flight Vulkan frame and by the displayed frame; FFmpeg reuses the surface only after the last release. The interop pool size is FFmpeg's recommendation + queue slots + `VulkanContext::framesInFlight` + displayed + spare.

**Interop policy.** `interop-copy` is the supported Windows fast path; global defaults stay portable. Neither explicit path may silently fall back to readback or to the other path. `--decoder auto` falls back to software and uses readback.

**Known driver issue.** On the tested NVIDIA RTX 3060/616.92, sampling imported multi-planar NV12 returns scrambled data or faults the GPU. Exact-profile import, Vulkan-owned export, and keyed-mutex variants were also rejected. Importing plain R8/R8G8 textures works. Direct Vulkan Video decode is a separate future phase; the current FFmpeg package lacks Vulkan support. See `docs/phase3/d3d11-vulkan-interop.md`.

**Vulkan resources.** There are `VulkanContext::framesInFlight` independent `FlightResources` sets (staging, plane images, RGBA16F working/intermediate/output), so one submitted frame never overwrites another's inputs. The color pass expands the range, then applies the matrix, and keeps nonlinear values in RGBA16F. With an sRGB swapchain, the present shader linearizes and the attachment does the single encode. EASU/RCAS/CAS shaders include the pinned AMD FidelityFX source from `third_party/fidelityfx-fsr1/`.

**Live bridge.** `external/SysDVR-UpscalerBridge` is a git submodule: a C# SysDVR fork, built with `scripts/build-sysdvr-upscaler-bridge.ps1` into `artifacts/sysdvr-upscaler-bridge/win-x64/`. It connects as a client to a byte-stream named pipe served by `SysDvrPipeSource`. The framed protocol (`SUBH` hello, `SUBP` packets, little-endian, never raw struct serialization) is specified in `docs/sysdvr-upscaler-bridge-protocol.md` and implemented and tested in `SysDvrBridgeProtocol`. `tools/SysDvrBridgeTestProducer` is a synthetic producer for testing without Switch hardware.

## Conventions

- Status claims are deliberately conservative. Phase 2 and Phase 3 are "implemented" but not hardware-validated: USB reconnect, visual comparison, and the 30-minute live soak are still pending. Don't describe anything as hardware-verified in docs, README, or commits unless that was actually run.
- When behavior changes, update the matching phase doc in `docs/` and the milestone table in `README.md`.
- Errors in initialization and FFmpeg setup propagate as contextual exceptions. The per-frame hot path does not use exceptions for control flow.
