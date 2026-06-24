# Compression Preprocessing

Compression-aware preprocessing is planned but not enabled in this pass.

The intended modes are:

- `off`
- `denoise-lite`
- `deblock-lite`
- `combined-lite`

These modes must be conservative, disabled by default, independently timed, and rejected as defaults if they smear HUD text, silhouettes, fine geometry, or flat stylized gradients.