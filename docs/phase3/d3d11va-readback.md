# Phase 3.1 D3D11VA Readback Decode

Phase 3.1 added an opt-in FFmpeg D3D11VA H.264 backend. At that point, decoded D3D11 hardware frames were transferred back to CPU memory and normalized into the existing owned planar YUV420 frame slots before Vulkan upload.

## Backend Selection

The default remains software decode:

```powershell
.\build\win-release\NexusStream60.exe --source sysdvr-pipe --pipe-name SysDVR-Upscaler.Video --quality-preset balanced --presentation exact --latency-profile balanced --live-frame-queue-depth 1
```

Request D3D11VA explicitly:

```powershell
.\build\win-release\NexusStream60.exe --source sysdvr-pipe --pipe-name SysDVR-Upscaler.Video --quality-preset balanced --presentation exact --latency-profile balanced --live-frame-queue-depth 1 --decoder d3d11va
```

Use `auto` to try D3D11VA and fall back to software if setup or decoder open fails:

```powershell
.\build\win-release\NexusStream60.exe --source sysdvr-pipe --pipe-name SysDVR-Upscaler.Video --quality-preset balanced --presentation exact --latency-profile balanced --live-frame-queue-depth 1 --decoder auto
```

## Expected Confirmation

Startup logs should include:

```text
Decoder backend request: d3d11va
Decoder backend: requested d3d11va, active d3d11va
Active decoder backend: d3d11va
```

Telemetry shows `Decoder backend: d3d11va` and a codec/format similar to `h264 / d3d11 -> CPU nv12`. Phase 3.2 also reports `Frame storage: CPU NV12` when the direct NV12 Vulkan upload path is active.

## Design Boundary

This is not zero-copy yet. The decode engine moves from CPU software decode to D3D11VA hardware decode, but frames still return to CPU memory. Phase 3.2 removes the extra CPU planar U/V conversion by uploading CPU NV12 directly into Vulkan Y and interleaved UV images before shader color conversion.
