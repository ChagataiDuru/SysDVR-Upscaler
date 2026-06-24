# Spatial upscalers

All modes consume the same nonlinear RGBA16F image produced by the existing limited-range BT.709 conversion. Output is RGBA16F at the requested reconstruction size.

| Mode | Character | Typical tradeoff |
|---|---|---|
| Nearest | Pixel-coordinate diagnostic | Aliasing and blockiness |
| Bilinear | Stable control | Soft thin edges and text |
| Bicubic Catmull-Rom | Crisper classical reconstruction | Mild overshoot |
| Lanczos2 | Strong fine-edge reconstruction | Ringing; local anti-ringing is on by default |
| Bilinear + CAS | Low-cost reconstruction plus official FidelityFX CAS | May amplify codec boundaries at high strength |
| Lanczos2 + CAS | Strong classical baseline | Highest halo/noise risk among classical modes |
| FSR1 EASU | Official edge-adaptive spatial upsampling | Cannot recover detail absent from 720p |
| FSR1 EASU + RCAS | EASU plus separate official RCAS pass | RCAS may amplify ringing or mosquito noise when pushed |

CAS and RCAS have separate 0-to-1 controls. Defaults are CAS 0.35 and RCAS 0.25. The RCAS UI value maps nonlinearly through official attenuation stops; see the third-party integration notes. A conservative default is deliberate: readable HUD edges without turning H.264 artifacts into decoration.

All algorithms use the same output-center mapping. Catmull-Rom and Lanczos2 clamp source fetches at boundaries. Lanczos2 optionally clamps the result to the local four-sample min/max neighborhood to limit overshoot.

## Phase 1.5 note

Chroma reconstruction now happens inside the YUV420P-to-RGB pass and is selectable with `--chroma-upscale bilinear|bicubic|lanczos2|edge-aware`. See `docs/chroma-reconstruction.md` for chroma-location mapping and validation cautions.

NIS is not exposed as a runtime upscaler until the official NVIDIA SDK source, config constants, coefficient resources, and license/version records are integrated. See `docs/third-party-notices.md`.
