# Upscaling Quality Options: Planning Brief

This is a single input document for an agent that has to plan the next image-quality phase of NexusStream60. It combines three things:

- the project constraints, checked against the code on branch `macos-port` as of 2026-09-14
- one web-search pass covering results since 2024
- the model's background knowledge

**How much to trust this document.** The full research run (`/research-deep`) has **not** been done. The per-item numbers and claims below have not been checked against primary sources and have not been measured in this repo. Each option has a confidence tag:

- **[verified-in-repo]**: checked in this codebase
- **[web]**: found in the web-search pass, source listed at the end
- **[model]**: background knowledge, check before relying on it

Before committing a plan to any option, re-check its **[web]** and **[model]** claims.

Related files: `outline.yaml` (31 research items, execution config) and `fields.yaml` (the comparison field schema) in this folder.

---

## 1. Goal and hard constraints

**Goal:** better visual quality when upscaling Switch SysDVR video from 720p to 1080p (and optionally 1440p or 4K), while keeping the app's current near-zero added latency.

| Constraint | Value / implication |
|---|---|
| Input | Decoded **H.264** frames, 720p (some docked titles 1080p), 30 or 60 fps. Compression artifacts (blocking, ringing, mosquito noise, banding) are baked into the frames. |
| No engine data | No motion vectors, depth, or camera jitter. Game temporal upscalers (FSR 2/3/4, DLSS, XeSS, MetalFX Temporal) can't be used as designed. |
| Scale ratio | 720p→1080p is **1.5×** and 720p→1440p is 2×. Upscalers limited to fixed 2×/4× need an extra downscale for 1080p output. |
| GPU budget | **≤ 5 ms for the upscale stage** at 720p→1080p (user decision). |
| Latency | No extra frames of delay in live mode (`FrameQueue` keeps only the latest frame). The render thread must not block on the CPU. |
| Reference hardware | Windows: NVIDIA RTX 3060, driver 616.92. macOS: Apple M2 via MoltenVK. |
| macOS headroom | Live M2 telemetry shows **GPU total ≈ 5.4 ms** already, with frame-time P99 33.4 ms ([verified-in-repo] `docs/phase4-macos-port.md`). The 5 ms upscale budget is much tighter on M-series than on the RTX 3060. |
| Portability | Global defaults must work on both platforms. Vendor-specific paths must be explicit and must never silently fall back (the same rule as the decoder paths in `CLAUDE.md`). |
| Claims | Don't describe anything as hardware-validated unless it was actually run (`CLAUDE.md` conventions). |

---

## 2. Current implementation (integration surface) [verified-in-repo]

### Pipeline
`VideoPipeline::prepareFrame` → `recordProcessing`:

1. The YUV→RGB compute pass (`yuv420p_to_rgb.comp` / `nv12_to_rgb.comp`) writes a **nonlinear RGBA16F** `working` image at source size. Chroma upsampling happens inside this pass (`ChromaUpscaleMode`: bilinear, bicubic, lanczos2, edge-aware).
2. `recordMode` runs one or two compute passes: `working → output`, or `working → intermediate → output`.
3. `beginPresent`/`endPresent` draw a full-screen triangle into an sRGB swapchain; the fragment shader linearizes and the attachment does the encode.

### Modes
- `enum class UpscaleMode { Nearest, Bilinear, BicubicCatmullRom, Lanczos2, BilinearCas, Lanczos2Cas, Fsr1Easu, Fsr1EasuRcas }` in `src/render/Upscaling.h`.
- The FSR1 source is AMD FidelityFX FSR1 v1.20210629, pinned in `third_party/fidelityfx-fsr1/`.

### Resource model (`src/render/VideoPipeline.h`)
- There are `VulkanContext::framesInFlight = 3` `FlightResources` sets. Each holds `y, u, v, uv, working, intermediate, output, comparisonOutput`.
- `intermediate`, `output` and `comparisonOutput` are RGBA16F at **output** size; `working` is at **source** size.
- The process descriptor layout is fixed: **binding 0 = one combined image sampler (input), binding 1 = one storage image (output)**. There are 5 prebuilt `processSets` (`WorkingToOutput`, `WorkingToIntermediate`, `IntermediateToOutput`, `WorkingToComparison`, `IntermediateToComparison`).
- `dispatchPass` always dispatches `(outputExtent + 15) / 16` groups with a 16×16 local size.
- `enum Pipeline { Nearest, Bilinear, Bicubic, Lanczos2, Cas, Easu, Rcas }`: one compute pipeline per shader, sharing `processPipelineLayout_`.

### Telemetry
- `enum class GpuPass { Nearest, Bilinear, Bicubic, Lanczos2, Cas, Easu, Rcas, Count }` is in `src/telemetry/Metrics.h`.
- `VulkanContext::timestampsPerFrame = 9`: 3 before processing, **up to 4 pass stamps** (`std::array<GpuPass, 4>`), and 2 for present.
- `PERF_SUMMARY` logs averages over the last 240 samples.

### Controls and config
- Hotkeys `1`–`8` map directly onto the 8 modes (`src/app/Application.cpp` ~L541), so **no number keys are free**.
- `[`/`]` adjust CAS, or RCAS when the mode is `Fsr1EasuRcas`.
- CLI (`src/app/AppConfig.cpp`): `--upscale`, `--cas-sharpness`, `--rcas-sharpness`, `--chroma-upscale`, and `--quality-preset balanced|performance|quality`. Both `balanced` and `quality` use `Fsr1EasuRcas`.
- Comparison mode is A/B split with a draggable divider; a zoom inspector exists; screenshots record the mode and timings.

### Build and tests
- Every new shader must be added to `ns60_compile_shaders(...)` in `CMakeLists.txt`.
- Mode round-trip tests are in `tests/UpscalingTests.cpp`, and CLI tests in `tests/AppConfigTests.cpp`. Both enumerate every mode, so update them when adding modes.

### Existing plans and rules
- `docs/upscalers.md`, `docs/third-party-notices.md`: **NIS must not be exposed until the official NVIDIA source, coefficient resources, license text and version record are vendored.** No approximations labeled `nis`.
- `docs/compression-preprocessing.md`: planned `off | denoise-lite | deblock-lite | combined-lite`. These must be conservative, off by default, timed separately, and rejected if they smear HUD text, silhouettes or flat gradients.
- `docs/nexus-adaptive-detail.md`: a planned project-specific luma detail enhancer using a confidence mask and anti-ringing clamps. Off by default and must not be labeled CAS/RCAS/NIS.
- `docs/upscale-comparison.md`: the validation table is mostly empty. There is **no measured quality evidence** comparing current modes yet.
- Project license: **MIT**. That affects which shader licenses can be vendored (see §5).

### Decoder paths (affects vendor video-SR integration)
- **Windows:** software → `CpuYuv420P`; `d3d11va` readback → `CpuNv12`; `d3d11va` `interop-copy` → D3D11 textures imported into Vulkan as R8/R8G8.
- **macOS:** software or VideoToolbox, both with **CPU NV12 readback**. There is no Metal/IOSurface zero-copy path today.

---

## 3. Option catalog

"Budget" is the expected fit to ≤ 5 ms at 720p→1080p; nothing has been measured yet. "Shape" is how the option would plug in:

- **pass**: new compute pass(es) in `recordMode`
- **multi**: needs a multi-input or multi-pass shader framework
- **interop**: needs a D3D11/D3D12/CUDA/Metal texture exchange
- **pre**: runs before the upscale

| # | Option | Kind | Platforms | 1.5× native | Budget | Shape | License | Tentative verdict | Conf. |
|---|---|---|---|---|---|---|---|---|---|
| 1 | FSR1 tuning (luma-aware EASU order, RCAS denoise, param retune) | spatial | all | yes | yes (<1 ms) | pass | MIT (AMD) | **Adopt**; cheap baseline gains | model |
| 2 | NVIDIA Image Scaling (NIS) v1.0.3 | spatial | all | yes | yes (~FSR1 class) | pass + coefficient textures | MIT | **Prototype** (official source only) | web/model |
| 3 | Snapdragon GSR1 | spatial | all | yes | yes (cheaper than EASU) | pass | BSD-3 | **Prototype** | web |
| 4 | MetalFX Spatial Scaler | spatial | macOS | yes | likely | interop (Metal↔Vulkan, `VK_EXT_metal_objects`) | Apple SDK | **Prototype (macOS)** | web |
| 5 | Deblock / dering / deband pre-pass | pre | all | n/a | yes if lite | pre pass | own / libplacebo-inspired | **Adopt**; biggest likely gain for H.264 | model |
| 6 | Chroma-from-luma (CfL) / KrigBilateral | chroma | all | n/a | yes (~1 ms) | inside color pass | verify (mpv shaders often LGPL/GPL) | **Prototype**; reimplement, don't copy | model |
| 7 | Anime4K (3D_Upscale_US, 3D_AA_Upscale_US, Restore) | CNN shader | all | 2× nets (+resample) | likely for small variants | multi | MIT | **Prototype** | web/model |
| 8 | CuNNy (veryfast…8x32, DS/SOFT) | CNN shader | all | 2× (+resample) | small tiers likely | multi | verify | **Prototype**; strong on text/UI | web |
| 9 | ArtCNN (C4F16/C4F32, DS/DN) | CNN shader (luma) | all | 2× (+resample) | C4F16 likely, C4F32 uncertain | multi | verify | **Prototype** | web |
| 10 | FSRCNNX | CNN shader (luma) | all | 2× | borderline | multi | verify | Watch; superseded by ArtCNN/CuNNy | model |
| 11 | RAVU / NNEDI3 | trained edge-directed | all | 2× | RAVU yes / NNEDI3 borderline | multi | LGPL-3.0 (verify) | Watch; license friction | model |
| 12 | libplacebo (as library) | renderer | all | yes | depends | replaces pipeline pieces | LGPL-2.1 | Reject as dependency; **use as algorithm reference** | web/model |
| 13 | Magpie effects | reference | Windows (HLSL) | — | — | reference only | GPL-3.0 (verify) | **Reference only**; don't copy code | web |
| 14 | NVIDIA RTX Video SDK 1.1 (VSR + artifact reduction) | video SR | Windows, RTX 20+ | verify | vendor-claimed low | interop (DX11/DX12/Vulkan/CUDA) | proprietary | **Prototype (Windows NVIDIA)** | web |
| 15 | D3D11/D3D12 VideoProcessor VSR extensions (NVIDIA/Intel) | driver VSR | Windows | arbitrary | low | interop via existing D3D11 path | driver | **Prototype**; mpv `d3d11vpp` precedent | web |
| 16 | NVIDIA Maxine VFX (Artifact Reduction + SR) | video SR (TensorRT) | Windows, RTX | **yes (1.5×)** | uncertain | interop (CUDA) | proprietary | Watch; heavier redistributable | web |
| 17 | Apple `VTLowLatencySuperResolutionScaler` (macOS 26) | video SR (ML) | macOS 26+ | **no (2×/4× only, per forum)** | uncertain on M2 | interop (CVPixelBuffer) | Apple SDK | Watch; needs 1440p then downscale | web |
| 18 | AMD AMF Video Upscaling / HQ Scaler | video SR | Windows AMD | yes | low | interop (D3D11) | AMF SDK (MIT headers, verify) | Watch; no AMD test hardware | web |
| 19 | Intel VPL AI SR | video SR | Windows Intel Arc | verify | unknown | interop | MIT (VPL, verify) | Watch | web |
| 20 | Windows App SDK AI VSR / Auto SR / DirectSR | OS SR | Windows | — | NPU/CPU only / DX12 temporal | — | — | **Reject** (wrong device/API/domain) | web |
| 21 | SPAN / Real-ESRGAN Compact (mpv-AnimeJaNai, TensorRT) | ML runtime | Windows RTX | 2× | **unlikely ≤ 5 ms on 3060** | interop + runtime | BSD/MIT models (verify) | Watch; use as quality ceiling reference | web |
| 22 | TensorRT for RTX / Windows ML / ONNX Runtime CoreML / ncnn-vulkan | runtime | varies | — | model-dependent | runtime + interop | varies | Watch; only if a custom tiny model is trained | web |
| 23 | NTIRE 2025/2026 ESR, AIM 2024 compressed VSR | research models | — | — | port-dependent | multi (hand-ported) | paper/code licenses | Reference for custom CNN design | web |
| 24 | Arm NSS + ML SDK for Vulkan (`VK_ARM_tensors`) | ML (temporal) | not NVIDIA/MoltenVK natively | — | — | — | open | **Watch (future)** | web |
| 25 | FSR 2 / 3.1 | temporal | all (3.1 Vulkan) | yes | ~2–4 ms | needs MVs + depth + jitter | MIT | **Reject for now** (§4) | model |
| 26 | FSR 4 (FidelityFX SDK 2.0) | temporal ML | DX12 signed DLLs only | — | — | — | proprietary | **Reject** (no Vulkan) | web |
| 27 | DLSS 4.5 / XeSS 3 / MetalFX Temporal | temporal | vendor | — | — | engine inputs | proprietary | **Reject** | web |
| 28 | Arm ASR | temporal | all (MIT, FSR2-derived) | yes | ~FSR2 | needs MVs | MIT | Watch; only portable temporal option | web |
| 29 | SGSR2 | temporal | all (Vulkan sample) | yes | cheaper than FSR2 | needs MVs | BSD-3 (verify) | Watch | web |
| 30 | MV sources: FFmpeg `export_mvs`; `VK_NV_optical_flow` / NVOF SDK 5 | MV input | sw-decode only / NVIDIA Ampere+ | — | NVOF extra cost | — | — | Enabler for 25/28/29 only | web |
| 31 | KosmicKrisp (LunarG Vulkan-on-Metal) | macOS driver | macOS 13+/15+ | — | — | loader/ICD swap | open source | Watch; may lift portability-subset limits | web |

**Reference designs:**
- **Moonlight-qt PR #1557** [web]: NVIDIA VSR via D3D12, AMD AMF, FSR1 fallback on Intel, MetalFX Spatial on macOS. Reports <1 ms typical and ~2 ms worst-case added latency at 1440p120. This is the closest match to this project's problem.
- **chiaki-ng v1.10** [web]: libplacebo Vulkan renderer with deband and upscaler presets.

---

## 4. Key reasoning a planner needs

1. **Artifacts matter more than the scaler.** Every edge-adaptive or CNN scaler sharpens whatever is in the source, including H.264 blocking and ringing. `docs/upscalers.md` already warns that CAS/RCAS amplify codec boundaries. A cheap, conservative clean-up pre-pass (plan in `docs/compression-preprocessing.md`) or a CNN variant with built-in denoise (CuNNy DS, ArtCNN DS/DN, Anime4K Restore) is likely to improve quality more than switching to a different spatial kernel. [model]
2. **Temporal game upscalers don't fit decoded video.**
   - Without per-frame subpixel jitter, accumulation gains little new detail. It mostly denoises and risks ghosting.
   - Decoder motion vectors (`export_mvs`) are block-level, only available with software decode, and noisy.
   - Hardware optical flow (`VK_NV_optical_flow`) works on NVIDIA only and adds cost and latency.
   - Revisit only if a spatial CNN path plateaus. [model]
3. **The 1.5× ratio rules out many 2× models.** CNN shaders are mostly trained for 2×. For 1080p output, run 2× to 1440p and downscale, or run 2× on luma and resample. Include that downscale in the budget. `VTLowLatencySuperResolutionScaler` is reportedly 2×/4× only [web]. Maxine lists 1.5× [web].
4. **Keep portable and vendor paths separate.**
   - A portable tier (spatial and CNN compute shaders) can be the default on both OSes.
   - Vendor tiers are explicit opt-ins that never silently fall back, matching the decoder-path policy: RTX VSR / D3D11 VideoProcessor VSR on Windows, MetalFX / VT SR on macOS.
5. **Vendor video SR fits the input side of the Windows pipeline.**
   - D3D11 VideoProcessor VSR works on NV12/RGB D3D textures before import. It fits naturally next to `interop-copy` in `FFmpegVideoReader` / `D3D11CopyRing`, but the output is RGB at output size, not NV12 at source size. That changes the Vulkan import format and bypasses the NS60 color pass, so colorimetry has to be revalidated.
   - The NVIDIA RTX 3060 + 616.92 driver issue affects **multi-planar NV12 import only**. Importing single-plane R8/R8G8/RGBA works [verified-in-repo, `CLAUDE.md`].
6. **macOS vendor paths need a Metal interop that doesn't exist yet.**
   - MetalFX Spatial and VT SR both need Metal textures or CVPixelBuffers. Today the macOS path reads NV12 back to the CPU.
   - Options: `VK_EXT_metal_objects` to export/import `MTLTexture` with MoltenVK, or run the SR before the Vulkan upload (VT SR on the decoded CVPixelBuffer, then read back RGB at output size).
   - The CPU readback option costs more bus traffic at 1080p/1440p.
7. **M2 headroom is small.** GPU total is already ~5.4 ms. Any CNN tier needs its own measurement on M2 and probably a lighter default preset for macOS.
8. **There is no quality evidence yet.** `docs/upscale-comparison.md` is mostly blank. Build the evaluation harness first (§6, Tier 0), or every later "improvement" is unmeasurable.

---

## 5. License guidance for porting (project is MIT)

| Source | License | Action |
|---|---|---|
| AMD FSR1/CAS, NVIDIA NIS, Snapdragon GSR1/2, Arm ASR | MIT / BSD-3 | Vendor under `third_party/<name>/` with notices and version, following the FSR1 pattern and `docs/third-party-notices.md`. |
| Anime4K | MIT [model; verify] | Can be vendored with notice. |
| ArtCNN, CuNNy, FSRCNNX weights and shaders | **verify** | Check before vendoring; weights may have separate terms. |
| RAVU / NNEDI3 (mpv-prescalers), KrigBilateral, many mpv user shaders | LGPL/GPL [model; verify] | Don't copy into the MIT tree. Reimplement from the paper or description, or keep as an optional external file. |
| Magpie | GPL-3.0 [web; unconfirmed] | Reference only. |
| libplacebo | LGPL-2.1 | Algorithm reference only, unless the project deliberately accepts a dynamic-link dependency. |
| RTX Video SDK, Maxine, AMF, VPL, MetalFX, VideoToolbox | vendor SDK terms | Check redistribution rules; keep optional and behind platform guards. |

---

## 6. Suggested roadmap skeleton (for the planner to refine)

Tiers are ordered by value versus risk. Each tier should update `docs/upscalers.md`, add or extend a phase doc, and update the README milestone table, per `CLAUDE.md`.

### Tier 0: measurement and infrastructure (prerequisite)
- **Repeatable capture matrix.** Fill `docs/upscale-comparison.md` using the three samples (`flat_color`, `ui_text`, `fast_motion`) at identical PTS, with every mode. Extend `scripts/run-samples.ps1` or add a `.sh` twin for macOS.
- **Objective metrics** against a reference. Options: encode native 1080p content to 720p H.264 at SysDVR-like bitrates, upscale, and compare with PSNR/SSIM/VMAF/FLIP. Keep this offline tooling separate from the app.
- **Timing capacity.** Generalize timing past 4 pass stamps (`std::array<GpuPass, 4>`, `timestampsPerFrame = 9`). Add P95/P99 GPU pass time to `PERF_SUMMARY`.
- **Mode selection past `1`–`8`.** Options include a modifier key, cycling, or an ImGui dropdown. Keep the mode lists in `Application.cpp`, `UpscalingTests.cpp` and the CLI help in sync, ideally from one table in `ns60_core`.

### Tier 1: portable, cheap, low risk
- Compression pre-pass `deblock-lite` / `deband-lite` (the existing plan), timed and off by default.
- Official NIS integration (pass + coefficient textures, vendored per notices rule).
- SGSR1 as a new mode.
- FSR1 retune: RCAS denoise flag, and optionally EASU on luma plus cheaper chroma.
- CfL chroma reconstruction as a new `ChromaUpscaleMode` (own implementation).

### Tier 2: portable CNN shaders
- **Framework:** a multi-input descriptor layout (N sampled + 1 storage). Also a source-size feature-map image pool per flight, allocated once and never per frame, and dispatch extents that aren't tied to `outputExtent_`.
- **Candidates:** CuNNy (fast/veryfast, DS), ArtCNN C4F16 (DS), Anime4K 3D variants. Handle 2× → 1080p by resampling.
- **Per-platform defaults:** RTX 3060 may take a heavier tier; M2 likely needs the lightest.
- **Weight bundling:** baked into GLSL constants (typical of mpv shaders) or a SPIR-V specialization/buffer.

### Tier 3: vendor opt-ins (explicit, no silent fallback)
- **Windows:** D3D11 VideoProcessor VSR (NVIDIA/Intel) on the `interop-copy` path, or the RTX Video SDK. Import RGBA single-plane output; colorimetry must match the NS60 color pass.
- **macOS:** MetalFX Spatial via `VK_EXT_metal_objects` interop. Separately evaluate VT low-latency SR, keeping in mind the 2×/4× limit and macOS 26 requirement.

### Tier 4: watch or reject (document only)
- **Temporal:** FSR2/3.1, Arm ASR, SGSR2 with `export_mvs` or NVOF.
- **ML runtimes:** TensorRT-RTX, Windows ML, CoreML, ncnn for heavier models.
- **Other:** Arm NSS, KosmicKrisp, FSR4, DLSS, XeSS, Auto SR.
- Revisit when Tier 2 quality plateaus or the platforms change.

---

## 7. Likely code touch points

| Area | Files | Change |
|---|---|---|
| Mode enum / parse / names | `src/render/Upscaling.h/.cpp`, `tests/UpscalingTests.cpp` | New modes; keep round-trip tests exhaustive |
| CLI / presets | `src/app/AppConfig.cpp/.h`, `tests/AppConfigTests.cpp`, README CLI section | New `--upscale` values, pre-pass flags, preset updates |
| Pass recording | `src/render/VideoPipeline.cpp` (`createDescriptors`, `createPipelines`, `writeDescriptors`, `recordMode`, `dispatchPass`) | Multi-input layouts, feature-map images, per-pass dispatch extent |
| Resources | `VideoPipeline.h` `FlightResources`, `enum Pipeline`, `ProcessSet` | Preallocated images per flight for CNN/pre-pass |
| Timing | `src/telemetry/Metrics.h` (`GpuPass`), `src/render/VulkanContext.h` (`timestampsPerFrame`, timing plan) | More stamps; new pass ids |
| Hotkeys / UI | `src/app/Application.cpp` (~L541), overlay | Selection beyond 8 modes; sharpness control per new mode |
| Shaders / build | `shaders/*.comp`, `CMakeLists.txt` `ns60_compile_shaders` | Register every new shader |
| Third party | `third_party/<lib>/`, `docs/third-party-notices.md` | Vendored sources, licenses, versions |
| Windows vendor SR | `src/decode/FFmpegVideoReader*`, `D3D11CopyRing`, `src/render/D3D11VulkanInterop.*` | VideoProcessor/RTX VSR stage, RGBA import |
| macOS vendor SR | `src/render/VulkanContext.cpp` (extensions), new Metal interop unit behind `__APPLE__` | `VK_EXT_metal_objects`, MetalFX dispatch |
| Docs | `docs/upscalers.md`, `docs/compression-preprocessing.md`, `docs/upscale-comparison.md`, new phase doc, README milestones | Keep status claims conservative |

Hot-path rules still apply: no per-frame allocation, all GPU resources created once, and no exceptions for control flow per frame.

---

## 8. Open questions to settle before or while planning

1. What is the actual SysDVR bitrate and GOP structure for typical titles? This sets how aggressive the deblocking has to be.
2. Is 1080p the main target, or should 1440p/4K displays shape the design? 2× CNNs are a natural fit for 1440p.
3. For the RTX 3060 and M2, what are the measured costs of NIS, SGSR1, CuNNy-fast and ArtCNN-C4F16 at 720p→1080p and →1440p? (This is the first deep-research or benchmark task.)
4. Does D3D11 VideoProcessor VSR run with the NVIDIA driver setting on by default, and what does it do to latency and colorimetry compared with the NS60 color pass?
5. Can MoltenVK on the target macOS versions export `MTLTexture` via `VK_EXT_metal_objects` for images the pipeline already allocates? Does KosmicKrisp change this?
6. What are the exact licenses of CuNNy, ArtCNN and FSRCNNX shaders and weights?
7. Is `VTLowLatencySuperResolutionScaler` really limited to 2×/4×, and what does it cost on M2?
8. Is a project-trained tiny CNN on real SysDVR captures (Tier 2+) worth the tooling investment compared with off-the-shelf shaders?

---

## 9. Sources (web-search pass)

- SGSR: https://github.com/SnapdragonGameStudios/snapdragon-gsr · SGSR2 blog: https://www.qualcomm.com/developer/blog/2024/10/introducing-snapdragon-game-super-resolution-2 · SGSR2 Vulkan sample: https://github.com/SnapdragonGameStudios/adreno-gpu-vulkan-code-sample-framework/tree/main/samples/sgsr2
- NIS: https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
- Arm ASR: https://github.com/arm/accuracy-super-resolution · Arm NSS: https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/how-to-access-arm-neural-super-sampling · ML SDK for Vulkan: https://developer.arm.com/mobile-graphics-and-gaming/ml-sdk-vulkan
- CuNNy: https://github.com/funnyplanter/CuNNy · ArtCNN: https://github.com/Artoriuz/ArtCNN · mpv scaler evaluation: https://artoriuz.github.io/blog/mpv_upscaling.html · Anime4K releases: https://github.com/bloc97/Anime4K/releases · Magpie effects: https://github.com/Blinue/Magpie/wiki/Built-in-effects · mpv-AnimeJaNai: https://github.com/the-database/mpv-AnimeJaNai
- libplacebo releases: https://github.com/haasn/libplacebo/releases · FFmpeg filters: https://ffmpeg.org/ffmpeg-filters.html · FFmpeg extract_mvs: https://ffmpeg.org/doxygen/8.0/extract_mvs_8c-example.html
- KosmicKrisp conformance: https://www.lunarg.com/lunarg-achieves-vulkan-1-3-conformance-with-kosmickrisp-on-apple-silicon/ · State of Vulkan on Apple (Jan 2026): https://www.lunarg.com/the-state-of-vulkan-on-apple-jan-2026/ · VK_EXT_metal_objects: https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_metal_objects.html
- Apple VT low-latency SR config: https://developer.apple.com/documentation/videotoolbox/vtlowlatencysuperresolutionscalerconfiguration · Forum (2×/4× constraint): https://developer.apple.com/forums/thread/814646 · WWDC25 ML video effects: https://developer.apple.com/videos/play/wwdc2025/300 · WWDC25 MetalFX: https://developer.apple.com/videos/play/wwdc2025/211/
- RTX Video SDK: https://developer.nvidia.com/rtx-video-sdk/getting-started · Maxine VFX SR: https://docs.nvidia.com/maxine/vfx/latest/Filters/SuperResolution.html · TensorRT for RTX: https://developer.nvidia.com/blog/nvidia-tensorrt-for-rtx-introduces-an-optimized-inference-ai-library-on-windows/ · VK_NV_optical_flow: https://developer.nvidia.com/blog/accelerated-motion-processing-brought-to-vulkan-with-optical-flow-sdk/
- mpv d3d11vpp VSR: https://github.com/mpv-player/mpv/commit/2848af5618fa823571cf4ec8cc2a4580d37f1648 · Moonlight-qt VSR PR: https://github.com/moonlight-stream/moonlight-qt/pull/1557 · chiaki-ng v1.10.0: https://github.com/streetpea/chiaki-ng/releases/tag/v1.10.0
- FFmpeg AMF / sr_amf: https://www.phoronix.com/news/FFmpeg-AMD-AMF-Decoder-FSR · FidelityFX SDK 2.0: https://videocardz.com/newz/amd-releases-fidelityfx-sdk-2-0 · DLSS 4.5: https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/ · XeSS 3.0.2: https://videocardz.com/newz/intel-xess-sdk-3-0-2-released-brings-updates-to-xe-low-latency
- DirectML maintenance: https://github.com/microsoft/DirectML · Windows App SDK AI VSR: https://learn.microsoft.com/en-us/windows/ai/apis/video-super-resolution · Auto SR: https://devblogs.microsoft.com/directx/autosr/
- AIM 2024 compressed VSR: https://arxiv.org/abs/2409.17256 · NTIRE 2025 ESR: https://arxiv.org/abs/2504.10686 · NTIRE 2026 ESR: https://arxiv.org/pdf/2604.03198 · CIAF: https://arxiv.org/abs/2210.08229
