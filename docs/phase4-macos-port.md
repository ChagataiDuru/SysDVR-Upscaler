# Phase 4.0 macOS (Apple Silicon) Port

Phase 4.0 brings NexusStream60 to macOS on Apple Silicon. All three source modes — recorded file, manual `sysdvr-pipe`, and managed `sysdvr` — use the same `ns60_core`, FFmpeg, and Vulkan pipeline as on Windows. Decoding is software or FFmpeg VideoToolbox with CPU NV12 readback. The Windows D3D11VA and D3D11/Vulkan interop paths are unchanged and stay Windows-only.

## Status

Implemented. Checked on an Apple M2 running macOS 15.7 with MoltenVK 1.4.1:

- `mac-core-tests`: all 61 unit tests pass, including the Unix socket path mapping and VideoToolbox CLI cases.
- `--decoder-capabilities`: MoltenVK is selected with the portability subset, and the VideoToolbox FFmpeg device is available with NV12 among its transfer formats.
- A generated 1280×720 60 fps limited-range BT.709 H.264 clip plays with Vulkan validation enabled and produces no validation messages under both `--decoder software` and `--decoder videotoolbox`. The frame-120 `fsr1-easu-rcas` captures from the two decoders are byte-identical.
- `--source sysdvr-pipe` fed by `tools/SysDvrBridgeTestProducer` over the Unix socket: the hello is parsed, VideoToolbox decodes, and frames are presented and captured. A stale socket file from an earlier killed run is replaced.
- `--source sysdvr` with a Switch connected over USB through a USB-C dock (Debug build, validation on, 1280×720 input reconstructed to 1920×1080 with FSR1 EASU + RCAS):
  - `--decoder software`: about 96 s of live play at a rolling 60 FPS with 0 dropped frames and no validation messages. P50/P95 active frame time was 16.8/25.5 ms.
  - `--decoder videotoolbox`: about 4.6 minutes of live play, including a full Mario Kart 8 race that the tester reported as feeling low-latency. Decode synchronized on the first keyframe, VideoToolbox reported no errors, and there were no validation messages. Telemetry after about 15,700 decoded frames: frame storage CPU NV12, lifetime presented FPS 59.08, 0 dropped / 170 repeated frames, 203 stale decoded frames dropped, frame-time P50/P95/P99 16.7/17.2/33.4 ms, and GPU total about 5.4 ms. Live `CPU decode` telemetry includes waiting for the next packet, so it reads about 16 ms for both decoders and does not measure decoder cost.
  - Both sessions ended with Escape: the app stopped the running bridge, no bridge process remained, and the socket file was removed.
- `--source sysdvr` without a console attached: the bridge exits after its USB retries, the app reports the empty stream, and the socket and bridge process are cleaned up.

Not yet run: USB disconnect/reconnect, visual comparison against Windows, a Release-build live session, and the 30-minute live soak. Do not treat this phase as fully hardware-validated.

## Build

```sh
export VCPKG_ROOT="$HOME/vcpkg"
source ~/VulkanSDK/<version>/setup-env.sh
cmake --preset mac-debug && cmake --build --preset mac-debug
cmake --preset mac-core-tests && cmake --build --preset mac-core-tests && ctest --preset mac-core-tests
./scripts/build-sysdvr-upscaler-bridge.sh
```

Presets carry a `hostSystemName` condition, so Windows shows only `win-*`/`core-tests` and macOS only `mac-*`. See [macOS dependencies](dependencies-macos.md) for the toolchain, the .NET 9 SDK, and libusb.

## Vulkan on MoltenVK

- On macOS the instance enables `VK_KHR_portability_enumeration` with `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`; without it the loader hides MoltenVK and no physical device is found. The capability report's own audit instance does the same. Other platforms keep their previous enumeration.
- The logical device enables `VK_KHR_portability_subset` whenever the device advertises it, as the specification requires.
- `Window` calls `glfwInitVulkanLoader(vkGetInstanceProcAddr)` before `glfwInit`. Otherwise GLFW dlopens `libvulkan` by name, which `setup-env.sh`'s `DYLD_LIBRARY_PATH` resolves to the SDK loader while the executable links vcpkg's. The two loaders' dispatch tables differ, so `glfwCreateWindowSurface` jumped into the wrong entry and crashed with `SIGSEGV` on the first live launch.
- MoltenVK offers FIFO and Immediate present modes. `--vsync off` falls back from Mailbox to Immediate through the existing selection logic.
- The executable links vcpkg's `vulkan-loader` (a dependency of `imgui[vulkan-binding]`), which discovers the MoltenVK ICD installed by the LunarG SDK. The startup log names the selected driver.
- On Retina displays `--width`/`--height` remain framebuffer pixels; the window is sized in points by dividing by the content scale. `--presentation exact` maps output pixels 1:1 onto physical pixels, so on a 2880×1800 panel a 1920×1080 image covers 960×540 points; use `fit` to fill the screen.

## Live bridge transport

The bridge opens its endpoint with .NET's `NamedPipeClientStream`, which is a Unix domain socket client on macOS, so the submodule needs no changes. `SysDvrPipeSource` serves the socket where .NET expects it (`sysdvr_bridge::unixSocketPathFor` in `ns60_core`):

- A plain name maps to `$TMPDIR/CoreFxPipe_<name>`, or `/tmp/CoreFxPipe_<name>` when `TMPDIR` is unset. The manual default `SysDVR-Upscaler.Video` therefore works unchanged on both sides.
- An absolute name is the socket path itself. Managed launches use `$TMPDIR/ns60-<pid>-<ticks>.sock`, which avoids the `CoreFxPipe_` prefix and keeps concurrent runs apart.
- Paths are limited to 103 bytes (the macOS `sun_path` size minus the terminator).

The server binds a `SOCK_STREAM` socket, restricts it to mode 0600, listens for one client, and accepts on the decoder thread just as `ConnectNamedPipe` does on Windows. A leftover socket file from an earlier run is replaced, any other file type at that path is refused, and the socket is unlinked on shutdown. Reads retry on `EINTR`; end of stream or `ECONNRESET` is a normal disconnect.

The managed launch uses `posix_spawn` with an argument vector, the bridge's directory as working directory, and its own process group, mirroring `CREATE_NEW_PROCESS_GROUP`. Stopping sends `SIGTERM` so the bridge can release the USB device, waits up to 3 s, then sends `SIGKILL`.

## VideoToolbox readback

- `--decoder videotoolbox` (alias `vt`) opens FFmpeg's VideoToolbox `hw_device_ctx`, prefers an NV12 transfer, and stores frames as `CpuNv12` for the existing `nv12_to_rgb.comp` upload. Telemetry shows `videotoolbox_vld -> CPU nv12`.
- An explicit `videotoolbox` request fails rather than falling back. `--decoder auto` tries VideoToolbox on macOS (D3D11VA on Windows) and falls back to software.
- `--decoder-path interop`/`interop-copy` still require `--decoder d3d11va` and are rejected on macOS.
- `FFmpegVideoReader::configureHardwareDecoder` replaces the D3D11VA-only setup; backend-specific names appear in logs and errors, and Windows messages are unchanged.
- VideoToolbox decodes with `thread_count = 1`. The media engine does the work, so frame threads only add latency and report a rejected frame on a later packet.

### Joining a live stream mid-GOP

The Switch's GRC encoder emits SPS/PPS once, and the sysmodule re-injects them into IDR frames, so a USB connection starts with P-frames whose references were never sent. The software decoder conceals them. VideoToolbox rejects them with `kVTVideoDecoderBadDataErr` (-12909, "output image buffer is null"), and FFmpeg then fails `avcodec_send_packet`, which ended the first live VideoToolbox session before any frame was shown.

For live input on VideoToolbox, `FFmpegVideoReader` now sets `skip_frame = AVDISCARD_NONKEY` until a frame decodes, then restores `AVDISCARD_DEFAULT` and logs `VideoToolbox live decode synchronized on a keyframe`. If VideoToolbox rejects data later, the reader logs a warning and waits for the next keyframe again, instead of ending the stream. More than 30 consecutive rejected packets without a decoded frame still end the stream with the original error. This recovery stays inside VideoToolbox and never switches decoders. File input and the Windows backends are unaffected.

## Platform notes

- libc++ only provides floating-point `std::from_chars` from macOS 26, so `AppConfig` parses the sharpness options with a strict decimal-only `strtof` fallback on Apple platforms.
- clang's `-Wextra` flags every Vulkan `{sType}` initializer through `missing-field-initializers`, so non-MSVC builds disable that one warning.
- `cmake/FindFFmpeg.cmake` creates `INTERFACE IMPORTED` targets instead of `ALIAS` targets, because vcpkg's FFmpeg wrapper adds Apple framework link flags to them.
- `scripts/run-samples.ps1` and `scripts/validate-samples.ps1` are PowerShell; install PowerShell with Homebrew to use them on macOS.
- On most Mac keyboards `F11` fullscreen needs `fn`+`F11`.
- When a live stream carries no data (for example, the bridge finds no console), FFmpeg probing fails and the app reports "YUV420P input dimensions must be positive and even". That message predates this phase and is misleading; the cause is an empty stream.

## Future improvements

Ordered by expected value, based on the live VideoToolbox telemetry above.

1. **Separate live input wait from decode time.** Live `CPU decode` (about 16 ms) includes blocking on the socket for the next packet, so it looks the same for software and VideoToolbox. Split it into an input-wait metric and a pure decode metric so decoder changes can be measured.
2. **Presentation pacing.** Over a full race, 170 of about 15,700 frames were repeated and P99 frame time was 33.4 ms, which means an occasional missed vsync under FIFO. Switch output and the display both run near 60 Hz, so small drift periodically lands two frames in one refresh interval. Candidates: measure with `--vsync off` (Immediate); pace presentation from display timing (`CAMetalLayer`/`CADisplayLink` through MoltenVK, or `VK_EXT_present_timing` where available); and on ProMotion displays, present at 120 Hz.
3. **Stale decoded frames.** 203 decoded frames were superseded before presentation. That is expected under latest-frame live semantics, but bridge USB arrival jitter isn't visible yet. Parse the bridge `Status` messages (currently ignored) into telemetry for queue depth, oldest payload age, and pipe write time.
4. **Honor bridge discontinuities.** `Discontinuity` messages are only logged. The protocol says the parent should flush decoder state; on VideoToolbox that flush should also re-arm the keyframe wait.
5. **VideoToolbox-to-MoltenVK zero-copy.** Import the decoder's `IOSurface`/`MTLTexture` through `VK_EXT_metal_objects` to remove the NV12 transfer, CPU plane copy, and staging upload. At 720p these CPU copies are under 1 ms, so this matters mostly for power and for higher resolutions. It is a separate phase with the same no-silent-fallback policy as the Windows interop paths.
6. **Retina presentation.** `--presentation exact` shows a 1920×1080 output as 960×540 points on a 2880×1800 panel. Consider HiDPI-aware integer scaling or a macOS default of `fit`, and reconstruction targets matched to Retina resolutions (for example 2880×1620).
7. **Hardware validation still owed.** A Release-build live session, USB disconnect/reconnect, visual comparison against Windows, and the 30-minute soak.
8. **Packaging.** Shaders load from the absolute `NS60_SHADER_DIR` in the build tree, and Vulkan relies on the SDK's ICD registration and `setup-env.sh`. A relocatable `.app` would bundle shaders, the loader, MoltenVK, and the bridge, with ad-hoc or Developer ID signing.
9. **Small cleanups.** Replace the misleading "YUV420P input dimensions" error for an empty live stream. Remove dead-process `ns60-*.sock` files at startup (managed socket names are unique, so a crashed run's socket is never reused). Drop the duplicate GLFW link reported by `ld`.
