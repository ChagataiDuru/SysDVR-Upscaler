# Third-Party Notices

## AMD FidelityFX FSR1/CAS

The existing AMD FidelityFX FSR1 and CAS integration remains pinned under `third_party/fidelityfx-fsr1/` with license notices preserved.

## NVIDIA Image Scaling

The official NVIDIA Image Scaling SDK is published at `https://github.com/NVIDIAGameWorks/NVIDIAImageScaling`. The repository currently identifies NVIDIA Image Scaling SDK v1.0.3 and MIT licensing.

Runtime NIS is not integrated in this pass. Do not expose an approximation as `nis`. A correct integration should vendor the official source files, preserve NVIDIA copyright/license text, record upstream commit/version, provide coefficient resources, and document Vulkan/GLSL binding/config adaptation.
## SysDVR-UpscalerBridge

`external/SysDVR-UpscalerBridge` is a Git submodule fork of SysDVR:

- Fork: https://github.com/ChagataiDuru/SysDVR-UpscalerBridge
- Upstream: https://github.com/exelix11/SysDVR

The bridge remains under the upstream SysDVR licensing terms. The parent project must keep this source boundary and provenance visible. The named-pipe bridge does not by itself resolve every possible licensing question.
