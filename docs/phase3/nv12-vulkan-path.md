# Phase 3.2 Native NV12 Vulkan Upload

Phase 3.2 keeps the Phase 3.1 D3D11VA readback boundary, but stops normalizing NV12 transfers into planar YUV420P on the CPU. FFmpeg D3D11VA readback now prefers CPU NV12, preserves the interleaved UV plane in owned frame slots, and lets Vulkan upload it as a two-plane source:

- Y plane: full-resolution `VK_FORMAT_R8_UNORM`.
- UV plane: half-resolution `VK_FORMAT_R8G8_UNORM` with interleaved Cb/Cr samples.

The existing planar YUV420P path remains available for software decode and any decoder transfer format that is not NV12.

## Runtime Path

Use the same explicit D3D11VA live command:

```powershell
.\build\win-release\NexusStream60.exe --source sysdvr-pipe --pipe-name SysDVR-Upscaler.Video --quality-preset balanced --presentation exact --latency-profile balanced --live-frame-queue-depth 1 --decoder d3d11va
```

And the bridge:

```powershell
.\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe usb --upscaler-video-pipe SysDVR-Upscaler.Video --upscaler-pipe-queue-messages 16 --upscaler-pipe-queue-bytes 1048576 --upscaler-pipe-max-age-ms 50 --no-audio
```

## Expected Confirmation

The live telemetry should show:

```text
Decoder backend: d3d11va
Codec / format: h264 / d3d11 -> CPU nv12
Frame storage: CPU NV12
```

The NV12 shader uses the same limited/full range handling, BT.601/BT.709/BT.2020 matrix selection, left-sited chroma coordinates, and selectable chroma reconstruction modes as the planar shader.

## Design Boundary

This is still a CPU readback path: D3D11VA frames are transferred out of the hardware decoder before Vulkan sees them. The Phase 3.2 win is that Vulkan now consumes native NV12 layout directly, avoiding CPU-side UV deinterleaving and keeping the shader color pipeline ready for the later Phase 3.3 D3D11/Vulkan interop path.
