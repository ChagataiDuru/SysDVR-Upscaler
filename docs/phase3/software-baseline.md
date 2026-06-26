# Phase 3 Software Baseline

Phase 3 starts from the Phase 2.1 live path: SysDVR bridge input, software FFmpeg H.264 decode, YUV420P CPU frame ownership, Vulkan upload/color conversion/upscale, and immediate live presentation. Hardware decode is not enabled by the Phase 3.0 capability report.

## Baseline Commands

```powershell
.\build\win-release\NexusStream60.exe --list-decoders
.\build\win-release\NexusStream60.exe --decoder-capabilities
```

For the live software baseline:

```powershell
.\build\win-release\NexusStream60.exe --source sysdvr-pipe --pipe-name SysDVR-Upscaler.Video --quality-preset balanced --presentation exact --latency-profile balanced --live-frame-queue-depth 1
```

```powershell
.\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe usb --upscaler-video-pipe SysDVR-Upscaler.Video --upscaler-pipe-queue-messages 16 --upscaler-pipe-queue-bytes 1048576 --upscaler-pipe-max-age-ms 50 --no-audio
```

## Phase 3.0 Acceptance

- `--list-decoders` returns the FFmpeg software decoder and compiled hardware-device inventory without requiring an input file.
- `--decoder-capabilities` reports D3D11VA device creation, H.264 hardware configs, D3D11VA transfer formats, the default D3D11 adapter, the selected headless Vulkan device, DXGI/Vulkan LUIDs where available, external-memory import signals, and external sync support.
- The report is observational only. The live pipeline remains on software decode until the D3D11VA backend is implemented and selected explicitly in a later Phase 3 step.

## Notes For Phase 3.1

The first hardware decode step should introduce a decoder backend selection without removing the software path. The expected first backend is D3D11VA decode with CPU readback into the existing YUV upload path, so behavior and latency can be compared before native NV12 Vulkan import is attempted.