# Upscale validation record

This table is evidence-only; blank cells have not been measured. The current automated session completed `flat_color_720p60.mp4` in FSR1 EASU + RCAS with validation enabled. A separate comparison launch was blocked by the tool execution quota, so no visual preference claim is made.

| Sample | Frame / PTS | Mode | Sharpness | GPU time | Edge clarity | Ringing | Compression amplification | Notes |
|---|---|---|---:|---:|---|---|---|---|
| flat_color_720p60 | Full clip | FSR1 EASU + RCAS | RCAS 0.25 | Not captured from headless log | Not formally scored | Not visually scored | Not formally scored | 33.651 s playback completed; validation output clean; borderless swapchain 1920x1080 |
| ui_text_720p60 | ? | Bilinear vs FSR1 EASU + RCAS | RCAS 0.25 | ? | ? | ? | ? | Built but runtime comparison not observed in this session |
| fast_motion_720p60 | ? | ? | ? | ? | ? | ? | ? | Pending full visual matrix |

Do not read the single completed smoke run as evidence that one algorithm is universally best. Use pause, split comparison, zoom, identical frame/PTS captures, and conservative sharpening across all three clips before choosing a default for a game.

## Phase 1.5 comparison guidance

Before comparing upscalers, first confirm telemetry reports `1:1 output mapping: Yes` and `Final resample: Inactive` for 1920x1080 reconstruction on a 1920x1080 framebuffer. If final resampling is active, the capture includes an extra presentation filter and should not be treated as a pure reconstruction comparison.
