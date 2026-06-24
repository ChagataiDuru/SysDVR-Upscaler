# Chroma Reconstruction

Nintendo Switch SysDVR samples are YUV420P: luma is full 1280x720, while Cb and Cr are 640x360. The verified samples report left-sited chroma.

For left-sited 4:2:0, a luma pixel `(x, y)` maps to chroma coordinates:

```text
cx = x * 0.5
cy = (y - 0.5) * 0.5
```

Horizontal chroma is co-sited with the left luma sample. Vertical chroma is centered between luma rows. This phase keeps that mapping explicit instead of assuming centered chroma.

Available modes:

- `bilinear`: baseline linear sampler reconstruction.
- `bicubic`: Catmull-Rom four-by-four reconstruction; default for validation.
- `lanczos2`: radius-2 reconstruction with conservative clamp against local bilinear-neighborhood extrema.
- `edge-aware`: experimental single-frame mode that uses luma gradients to avoid pushing chroma across strong luma edges.

Chroma sharpening is deliberately not applied independently. The pipeline remains Y/Cb/Cr reconstruction, limited-range BT.709 conversion, spatial reconstruction, optional sharpener, presentation.