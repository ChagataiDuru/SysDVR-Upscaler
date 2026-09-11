# Phase 3.3 D3D11/Vulkan Zero-Copy Interop Plan

Phase 3.3 will remove the remaining CPU readback and Vulkan staging upload from the D3D11VA path. The existing Phase 3.2 CPU NV12 path remains the default and the comparison baseline. Interop is selected explicitly with `--decoder d3d11va --decoder-path interop`; an explicit interop request fails rather than silently falling back.

The CLI and configuration type are scaffolded in this phase, but interop is not active yet. Until the implementation lands, selecting `interop` returns a clear startup error.

## Scope and Preconditions

The first interop implementation is Windows-only, D3D11VA-only, same-adapter-only, and limited to 8-bit NV12 H.264 frames. P010, cross-GPU copies, audio, D3D12VA, and other decoder backends are out of scope.

Startup must verify all of the following before decoding:

- FFmpeg selects `AV_PIX_FMT_D3D11` with an NV12 software format.
- The D3D11 device and selected Vulkan physical device have identical adapter LUIDs.
- Vulkan supports `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32`, timeline semaphores, `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`, and `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT`.
- `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` supports sampled imported images and compatible single-plane views.

Any failed precondition is fatal for explicit interop and must name the missing capability. `readback` behavior is unchanged.

## Decoder Surface Pool

Create the D3D11VA device first and expose its adapter LUID to Vulkan device selection. Before `avcodec_open2`, call `avcodec_get_hw_frames_parameters`, select NV12, and configure the returned `AVHWFramesContext`:

- Preserve FFmpeg's recommended decoder pool size, then add the decoded queue slot count, `VulkanContext::framesInFlight`, and one spare surface.
- Set `AVD3D11VAFramesContext::BindFlags` to include `D3D11_BIND_DECODER`.
- Set `AVD3D11VAFramesContext::MiscFlags` to include `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`.
- Initialize the pool with `av_hwframe_ctx_init` and assign it to `AVCodecContext::hw_frames_ctx`.

The decoded frame supplies an `ID3D11Texture2D*` in `AVFrame::data[0]` and its array slice in `AVFrame::data[1]`. No `av_hwframe_transfer_data` call occurs in interop mode.

## Vulkan Import and Views

Query `IDXGIResource1` from the decoder array texture and create an NT shared handle. Import the entire texture once as a Vulkan image using:

- `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`.
- The decoder width and height, one mip, and the D3D11 texture's array size.
- `VK_IMAGE_USAGE_SAMPLED_BIT`, `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`, and `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`.
- `VkImportMemoryWin32HandleInfoKHR`, plus dedicated allocation when required by the external-image query.

Create a pair of 2D views for every array slice: `VK_FORMAT_R8_UNORM` with `VK_IMAGE_ASPECT_PLANE_0_BIT` and `VK_FORMAT_R8G8_UNORM` with `VK_IMAGE_ASPECT_PLANE_1_BIT`. Cache imports by D3D11 texture identity and array slice. Both planes stay in `VK_IMAGE_LAYOUT_GENERAL` while shared externally and feed the existing NV12 conversion shader.

## Synchronization and Ownership

Create a shared `ID3D11Fence` through `ID3D11Device5`, export its NT handle, and import it as a Vulkan timeline semaphore. After `avcodec_receive_frame`, enqueue `ID3D11DeviceContext4::Signal(fence, ++readyValue)`; the frame packet carries that value.

Replace the CPU-only queued payload with a move-only decoded-frame packet that can contain either the current owned CPU planes or a D3D11 frame lease. A D3D11 lease contains metadata, texture identity, array slice, ready value, and an FFmpeg frame reference.

The Vulkan submission waits for the packet's ready value before sampling. The matching Vulkan flight retains the lease until its fence completes; only then may FFmpeg reuse that decoder surface. Frames discarded by the latest-frame queue release their leases immediately because they were never submitted.

Teardown order is strict: stop decode, wait for Vulkan flights, destroy plane views and imported memory, close owned Win32 handles, release frame leases and the FFmpeg pool, then release the D3D11 device.

## Renderer and Telemetry

Interop frames bypass `prepareFrame` CPU copies and all buffer-to-image uploads. Their Y/UV plane views bind to the existing NV12 descriptor layout and shader; color range, matrix, chroma reconstruction, upscaling, comparison, screenshots, and presentation remain unchanged.

Telemetry and logs add:

```text
Decoder path: d3d11-vulkan-interop
Frame storage: D3D11 NV12
CPU frame copy: 0 ms
Interop wait: <measured ms>
```

The capability report must distinguish extension presence from successful texture/fence import and report the selected adapter LUIDs.

## Validation and Acceptance

Automated tests cover CLI defaults and conflicts, LUID matching, import-cache keys, move-only lease lifetime, latest-frame drops, and flight-retained releases. A Windows integration test decodes a recorded H.264 sample through both paths and exercises shared texture and fence creation without requiring a Switch.

Manual acceptance compares `--decoder-path readback` and `--decoder-path interop` on the same frames. Captured RGB output may differ by no more than one 8-bit level per channel, validation layers must remain clean, queue pressure must not cause surface corruption or deadlock, and telemetry must show that CPU NV12 copy/upload work is absent. Complete USB reconnect testing and a 30-minute live soak before changing the default path.
