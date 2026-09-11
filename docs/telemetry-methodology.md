# Telemetry Methodology

Timing uses `std::chrono::steady_clock` for playback and CPU frame intervals.

Displayed FPS values are intentionally separated:

- Instant FPS: reciprocal of the latest active present-submission interval.
- Rolling FPS, 1 second: newest active present-submission intervals whose sum covers approximately one second.
- Rolling FPS, 120 frames: reciprocal of the average interval in the fixed history.
- Lifetime active presented FPS: active presented intervals divided by active playback seconds.
- Decoder throughput FPS: decoded frames per wall-clock sampling window.

Paused time is excluded from active playback histories. On resume, the near-term active interval anchor is reset so the pause does not create a large frame-time sample. Loop and seek reset active playback histories because their PTS anchors are discontinuous.

Present interval metrics are labeled as present submission intervals. They are not scan-out latency because the application does not measure actual display completion.

Percentiles are computed from fixed-capacity histories by copying into a fixed local array and sorting the populated region. Normal recording does not allocate.

Decoder telemetry separates CPU work from GPU work. `CPU plane copy` measures owned-plane copying on readback paths; for `interop-copy` the same row is labeled `CPU D3D copy submit` and measures API submission time. CPU copy/upload byte counters remain zero for D3D11/Vulkan interop. Screenshot readback is reported and allocated independently. The imported D3D11 fence is waited by a Vulkan queue submission, so no CPU duration is fabricated for it: the overlay reports the interop semaphore wait as `N/A (asynchronous timeline wait)`.

Normal shutdown also logs `PERF_SUMMARY` using the newest 240 samples. For `interop-copy`, `cpu_copy_submit_avg_ms` measures CPU time issuing the D3D copy, plane-split dispatch, and fence signal; it is not the asynchronous D3D GPU duration. `gpu_total_avg_ms` contains Vulkan timestamp work only.
