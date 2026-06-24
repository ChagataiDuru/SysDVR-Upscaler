# Presentation mapping

The reconstruction output, GLFW window, framebuffer, Vulkan swapchain, and content viewport are distinct sizes.

`--width` and `--height` request the reconstruction size and the initial client area. GLFW's actual framebuffer size is queried for every frame and Vulkan uses it for swapchain creation. Windows can clamp a decorated 1920x1080 window to its work area; on the validation machine that produced 1920x1061. Borderless fullscreen produced a true 1920x1080 framebuffer.

When reconstruction and framebuffer sizes match, presentation uses integer texel fetches: one reconstructed texel maps to one framebuffer pixel. Otherwise Fit (default), Fill, Exact, or Integer computes an aspect-preserving viewport and the present shader performs one documented linear resample. Telemetry reports both dimensions, the viewport, `1:1 output mapping`, and whether final resampling is active.

- Exact: one output texel per framebuffer pixel, centered; crops if necessary.
- Fit: preserve all content with letter/pillar boxes.
- Fill: preserve aspect ratio while cropping overflow.
- Integer: use the largest fitting integer scale when at least 1x, otherwise Fit behavior.

## Phase 1.5 update

Presentation geometry is now represented explicitly as requested reconstruction, framebuffer, swapchain extent, viewport, effective presentation mode, exact 1:1 mapping, and final-resample state. If no presentation mode is forced, the runtime chooses Exact for matching reconstruction/framebuffer dimensions and Fit otherwise.

`--final-filter nearest|bilinear` controls only active final resampling. True 1:1 presentation uses texel fetch and bypasses linear sampling.
