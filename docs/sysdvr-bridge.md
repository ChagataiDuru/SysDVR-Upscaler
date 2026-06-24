# SysDVR Bridge Integration

This is the short overview for the Phase 2 live bridge. The canonical wire-format reference is `docs/sysdvr-upscaler-bridge-protocol.md`; do not duplicate protocol tables here.

`SysDVR-Upscaler` keeps SysDVR USB transport isolated in the `external/SysDVR-UpscalerBridge` submodule. The bridge process owns the existing SysDVR USB source, handshake, packet validation, replay resolution, and reconnect behavior. The parent application owns named-pipe intake, FFmpeg H.264 decode, frame queueing, Vulkan reconstruction, presentation, screenshots, and telemetry.

Manual mode starts the parent first:

```powershell
.\build\win-release\NexusStream60.exe `
  --source sysdvr-pipe `
  --pipe-name SysDVR-Upscaler.Video `
  --quality-preset balanced `
  --presentation exact
```

Then start the bridge:

```powershell
.\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe usb `
  --upscaler-video-pipe SysDVR-Upscaler.Video `
  --no-audio
```

Managed mode lets the parent launch the published bridge:

```powershell
.\build\win-release\NexusStream60.exe `
  --source sysdvr `
  --sysdvr-bridge ".\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe" `
  --quality-preset balanced `
  --fullscreen `
  --borderless `
  --presentation exact
```