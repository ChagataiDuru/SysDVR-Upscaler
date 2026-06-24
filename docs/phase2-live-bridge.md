# Phase 2 Live Bridge

Phase 2 adds a live SysDVR path without copying SysDVR transport code into the parent C++ application.

Implemented pieces in this workspace:

- Bridge CLI option `--upscaler-video-pipe <name>`.
- Bridge protocol hello `SUBH` and stream messages `SUBP`.
- Bounded asynchronous bridge writer with immediate drop-on-overflow behavior.
- Parent named-pipe server source for protocol-framed H.264 payloads.
- Parent raw H.264 FFmpeg input path for live pipe mode.
- Live latest-frame decoded queue policy.
- Manual and managed run scripts.
- Synthetic producer scaffold for protocol testing without Switch hardware.

Validation stages remain explicit:

| Stage | Status |
| --- | --- |
| Unmodified bridge baseline | Not yet verified in this pass |
| Wire protocol without Switch | Implemented, not run in this pass |
| Recorded H.264 live path | Implemented path, not run in this pass |
| Manual live SysDVR | Requires hardware |
| Managed launch | Implemented path, not run in this pass |
| USB disconnect/reconnect | Requires hardware |
| Thirty-minute soak | Requires hardware |

Do not treat this document as a hardware-success claim.