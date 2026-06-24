# Phase 1.5 Quality Hardening

Phase 1.5 currently hardens presentation mapping, timing telemetry, and YUV420 chroma reconstruction without replacing the existing Vulkan renderer.

Implemented in this pass:

- Distinct presentation geometry for requested reconstruction, framebuffer, swapchain extent, viewport, effective presentation mode, final resample state, and exact 1:1 output mapping.
- Monitor-aware fullscreen and borderless window creation through `--monitor <index>`.
- Automatic presentation mode: Exact when framebuffer and reconstruction match, Fit otherwise unless `--presentation` is explicit.
- Explicit final presentation filter control through `--final-filter nearest|bilinear`; exact 1:1 presentation still uses texel fetch.
- Fixed-capacity timing histories with current, rolling, lifetime active FPS, frame-time percentiles, and present submission interval percentiles.
- Pause/seek/loop handling for active playback telemetry: paused time is not inserted as a giant frame interval.
- Selectable chroma reconstruction through `--chroma-upscale bilinear|bicubic|lanczos2|edge-aware`, with left-sited 4:2:0 mapping preserved.

Not yet claimed complete:

- Runtime NVIDIA Image Scaling mode. The official SDK v1.0.3 requires its config path and coefficient resources; do not label an approximation as NIS.
- Compression-aware preprocessing passes.
- Nexus Adaptive Detail pass.
- Preset plumbing.
- Repeatable multi-mode comparison capture session and motion-difference view.

Build note:

Use a Visual Studio x64 developer environment. The working non-interactive command is:

```powershell
cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && powershell -NoProfile -ExecutionPolicy Bypass -Command ""Set-Location -LiteralPath 'C:\Users\duruc\Documents\SwitchProject'; & 'C:\Program Files\CMake\bin\cmake.exe' --build 'build\win-release' --config Release --target NexusStream60"""
```