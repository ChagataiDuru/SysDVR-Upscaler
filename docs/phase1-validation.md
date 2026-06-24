# Phase 1 validation

Run `scripts/validate-samples.ps1` before application testing. Then use a Debug validation build for the complete clip and repeat with Release. Compare a captured frame with an FFmpeg PNG reference, paying special attention to code-16 black, code-235 white, neutral grays, fine UI text, chroma edges, gamma, and aspect ratio.

| Check | ui_text_720p60 | fast_motion_720p60 | flat_color_720p60 |
|---|---:|---:|---:|
| ffprobe metadata matches baseline | ☐ | ☐ | ☐ |
| decode-to-null integrity | ☐ | ☐ | ☐ |
| full-duration playback | ☐ | ☐ | ☐ |
| loop boundary clean | ☐ | ☐ | ☐ |
| pause/resume/step/Home | ☐ | ☐ | ☐ |
| resize/fullscreen/minimize | ☐ | ☐ | ☐ |
| no validation messages | ☐ | ☐ | ☐ |
| screenshot/reference color | ☐ | ☐ | ☐ |
| stable PTS pacing | ☐ | ☐ | ☐ |

Record rolling steady-state averages after shader/pipeline warm-up:

| Sample | CPU decode ms | CPU copy ms | GPU upload ms | GPU YUV→RGB ms | GPU bilinear ms | GPU present ms | GPU total ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| ui_text_720p60 | — | — | — | — | — | — | — |
| fast_motion_720p60 | — | — | — | — | — | — | — |
| flat_color_720p60 | — | — | — | — | — | — | — |

Acceptance also requires a clean exit while decoding, paused, and minimized; a bounded queue high-water no greater than four; no recurring project-owned hot-path allocations; correct behavior on a non-60 Hz monitor; and readable failures for an absent file, unsupported format, missing FFmpeg development package, missing shader compiler, or unsuitable Vulkan device.

