# SysDVR-Upscaler Bridge Integration

`SysDVR-Upscaler` keeps live Switch transport in the separate `SysDVR-UpscalerBridge` process. The bridge remains a fork of GPL-licensed SysDVR and continues to use the existing SysDVR USB implementation, handshake, packet validation, replay resolution, and reconnect behavior.

Repositories:

| Component | URL |
| --- | --- |
| Parent | https://github.com/ChagataiDuru/SysDVR-Upscaler |
| Bridge fork | https://github.com/ChagataiDuru/SysDVR-UpscalerBridge |
| Official upstream | https://github.com/exelix11/SysDVR |

Submodule path: `external/SysDVR-UpscalerBridge`.

## Build

Build the parent normally with CMake. Build the bridge explicitly:

```powershell
.\scripts\build-sysdvr-upscaler-bridge.ps1 -Configuration Release
```

Published bridge output goes to:

```text
artifacts/sysdvr-upscaler-bridge/win-x64
```

The normal CMake build does not rebuild the SysDVR fork.

## Manual Mode

Start the parent first:

```powershell
.\build\win-release\NexusStream60.exe `
    --source sysdvr-pipe `
    --pipe-name SysDVR-Upscaler.Video `
    --quality-preset balanced `
    --presentation exact
```

Start the bridge in another terminal:

```powershell
.\external\SysDVR-UpscalerBridge\Client\bin\Debug\net9.0\SysDVR-Client.exe usb `
    --upscaler-video-pipe SysDVR-Upscaler.Video `
    --no-audio
```

## Managed Mode

```powershell
.\build\win-release\NexusStream60.exe `
    --source sysdvr `
    --sysdvr-bridge ".\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe" `
    --quality-preset balanced `
    --fullscreen `
    --borderless `
    --presentation exact
```

The parent generates a unique pipe name and launches the bridge with `usb --upscaler-video-pipe <pipe> --no-audio`.

## Boundary

The bridge handles SysDVR transport. The parent handles H.264 parsing/decoding, YUV upload, Vulkan reconstruction, quality profiles, presentation, screenshots, and telemetry. Separate-process IPC keeps the provenance boundary visible; it is not a blanket licensing conclusion.

## Known Limitations

Hardware USB streaming, USB reconnect, and long soak behavior still require live Switch validation. Audio, hardware decode, shared memory, RTSP, and A/V synchronization are intentionally out of scope for this phase.