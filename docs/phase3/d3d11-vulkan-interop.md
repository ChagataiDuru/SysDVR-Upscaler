# Phase 3.3 D3D11/Vulkan NV12 Interop

Phase 3.3 is closed with the explicit `interop-copy` path as the supported Windows fast path. Strict import of a D3D11VA multi-planar NV12 decoder surface is not presented as fixed: `--decoder-path interop` exits with an explanatory unsupported error and never falls back silently. The portable default remains `readback`.

## Decision

On the tested NVIDIA GeForce RTX 3060 Laptop GPU with driver 616.92:

- `interop-copy` is pixel-exact against D3D11VA readback on `flat_color`, `ui_text`, and `fast_motion` at frame 60. Every run used Vulkan validation and reported a maximum channel difference of 0.
- The 58-test core suite passes.
- The strict imported-NV12 path corrupts sampled data or loses the Vulkan device. Enabling the supported `VK_EXT_ycbcr_image_arrays` extension and feature fixes the layered-image spec violation but not the driver behavior.
- Direct Vulkan Video H.264 decode is the next true-zero-copy candidate and belongs to a separate phase because the current vcpkg FFmpeg build has no Vulkan hardware-device support.

## Measured Cost

Release measurements used `flat_color_720p60.mp4`, 1280×720 input, 1920×1080 balanced EASU+RCAS output, exact presentation, validation off, vsync off, and the final 240 samples:

| Metric | D3D11VA readback | `interop-copy` |
| --- | ---: | ---: |
| CPU decode/readback average | 2.571 ms | 0.297 ms |
| CPU copy/interop-submit average | 0.097 ms | 0.163 ms |
| CPU Vulkan upload average | 0.106 ms | 0.000 ms |
| GPU Vulkan upload average | 0.232 ms | 0.000 ms |
| GPU color average | 0.123 ms | 0.150 ms |
| GPU Vulkan frame total average | 1.024 ms | 0.926 ms |
| Active frame P50 / P95 | 16.692 / 17.892 ms | 16.645 / 17.781 ms |
| CPU copy / upload bytes per frame | 1,382,400 / 1,382,400 | 0 / 0 |

`cpu_copy_submit_avg_ms` on `interop-copy` is CPU time spent issuing the D3D copy, plane-split dispatch, and fence signal. The small D3D GPU pass is asynchronous and is not included in the Vulkan timestamp total, so these values must not be read as a direct isolated GPU duration for that pass. End-to-end 60 Hz pacing was unchanged, while CPU readback and upload were eliminated.

Normal application shutdown logs a machine-readable `PERF_SUMMARY` line so the comparison can be repeated without reading the overlay.

## Supported GPU-Resident Path

Select the accepted path explicitly:

```powershell
.\build\win-release\NexusStream60.exe `
  --input .\samples\ui_text_720p60.mp4 `
  --decoder d3d11va --decoder-path interop-copy
```

Per decoded frame:

1. D3D11 copies the selected decoder-array slice into a private NV12 texture.
2. A small D3D11 compute shader splits it into shared R8 luma and R8G8 chroma textures.
3. Vulkan imports those ordinary single-plane images once and waits on the imported D3D11 fence as a timeline semaphore.
4. The existing NV12 color shader and all upscalers consume the shared planes unchanged.

No CPU pixel readback, CPU plane copy, or Vulkan staging upload occurs. The lease stays alive through the Vulkan flight fence, and the ring slot is not reused until its final lease is released. The render-complete semaphores are allocated per swapchain image, avoiding reuse while presentation still owns a wait.

## Why Strict Imported NV12 Is Unsupported

The following candidates were isolated in separate application runs so one `VK_ERROR_DEVICE_LOST` could not poison the next result:

1. Importing the D3D11VA decoder array as layered `G8_B8R8_2PLANE_420` succeeds, but sampling its R8/R8G8 plane views produces block-scrambled output and can lose the device.
2. Adding the exact H.264 8-bit 4:2:0 video profile, R8/R8G8 format list, and sampled/decode-destination/DPB usage produces no compatible Vulkan/D3D external-memory type on this driver.
3. A Vulkan-owned dedicated/exportable NV12 decoder array is rejected because the exact image configuration is not exportable as `D3D11_TEXTURE`.
4. A D3D11 `SHARED_NTHANDLE | SHARED_KEYEDMUTEX` decoder array can be created, but Vulkan import again exposes no compatible memory type. Removing Vulkan video usage does not change that result.

Imported images were kept in `GENERAL` layout with explicit external↔graphics ownership barriers, and the shared D3D11 decode fence remained in place. An isolation run that retained import, barriers, and synchronization but did not sample the planes completed without a device loss. The failure is therefore specific to sampling the imported multi-planar resource, not evidence that the 2019 Y′CbCr array extension is absent.

Failed Vulkan-owned and keyed-mutex candidates are not selectable production strategies. `interop-copy` remains explicit and never masquerades as strict zero-copy.

## Validation

Enable the opt-in Windows GPU tests with `NS60_ENABLE_GPU_TESTS=ON`. If local sample files exist, CTest registers `flat_color`, `ui_text`, and `fast_motion` interop-copy comparisons. Each test runs readback and interop-copy in separate processes, with validation enabled, then requires a maximum per-channel difference of 1 or less.

```powershell
cmake --preset win-debug -DNS60_ENABLE_GPU_TESTS=ON
cmake --build --preset win-debug
ctest --test-dir build/win-debug -R d3d11_vulkan_interop_copy --output-on-failure
```

## Switch Acceptance Still Open

Recorded-file acceptance is complete. Switch USB interaction and soak testing remain hardware work:

```powershell
.\build\win-release\NexusStream60.exe `
  --source sysdvr `
  --sysdvr-bridge ".\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe" `
  --decoder d3d11va `
  --decoder-path interop-copy `
  --quality-preset balanced `
  --presentation exact `
  --latency-profile balanced `
  --live-frame-queue-depth 1 `
  --validation on
```

Acceptance remains: five minutes of clean video and telemetry; pause/step, upscaler, resize/minimize/fullscreen and screenshot checks; a ten-second USB disconnect/reconnect; then a 30-minute validation-off soak. CPU copy/upload byte counters must stay zero and no Vulkan/NVIDIA error may appear.

## Future True Zero-Copy

The preferred follow-up is Vulkan Video decode: rebuild FFmpeg with Vulkan support, give its `AVVulkanDeviceContext` the renderer's Vulkan device/queues, and decode H.264 directly into Vulkan-owned images. Readback remains the portable fallback and `interop-copy` remains the proven Windows fallback. Vulkan Video must pass the same pixel comparisons and live Switch acceptance before becoming a default.
