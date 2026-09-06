# FSR RR captured-frame replay

Diagnostic tool, not a game mod. Runs one fresh-context RESET dispatch through a
locally supplied AMD RR 1.1 provider, on an explicitly named adapter. It neither
starts the game nor installs DLLs. Raw captures/vendor DLLs are never uploaded to CI.

Build with the existing **Build (No Signing)** GitHub workflow. Its contract-test
target also builds this executable into the artifact's `Diagnostics` directory.
Download the archive using `gh`. Keep the executable outside the game directory, so it does not load
the injected OptiScaler DXGI proxy.

Prepare jobs on a machine with Python and NumPy:

```sh
python tools/fsrrr-replay/prepare.py CAPTURE NEW-linear-job --encoding linear
python tools/fsrrr-replay/prepare.py CAPTURE NEW-sqrt-job --encoding sqrt
```

Run the downloaded Windows executable (native D3D12, or the existing Proton
runtime/DXVK/VKD3D installation) for each job:

```text
fsrrr-replay.exe NEW-linear-job/job.json ABSOLUTE-PATH/amd_fidelityfx_denoiser_dx12.dll NEW-linear-result
fsrrr-replay.exe NEW-sqrt-job/job.json ABSOLUTE-PATH/amd_fidelityfx_denoiser_dx12.dll NEW-sqrt-result
```

Do not replace any game or prefix files for this test. Required DXVK/VKD3D DLLs
may be copied alongside the replay executable when using Wine. Results are tightly
packed little-endian RGBA FP16, plus JSON recording the provider/default settings,
camera/dispatch data, source manifest and input hashes. Output paths must be new.

The baseline restores native texture formats from shader-visible values and
checks repacking error. Sqrt mode sqrt-encodes **all three albedos**, requantizes
to RGB10, and clears NON_GAMMA_ALBEDO, matching the SDK 2.2 sample's representation.
Radiance/depth/motion/normals do not change. The resulting small albedo quantization
difference is recorded; this is not an exactly equivalent mathematical input.

Limits: old manifests lack exact frame duration and live tuning settings. The tool
uses an explicit assumed duration (default 16.667 ms) and records provider defaults.
Camera delta is reconstructed from float32 matrices. A fresh context with RESET
is not guaranteed bit-identical to resetting an existing context. Compare the
linear replay with the captured reset result before drawing conclusions. A single
frame does not validate temporal quality or moving-object reprojection.

Reference: AMD FidelityFX SDK 2.2 `Samples/Denoisers/FidelityFX_Denoiser/shaders/`
`trace_rays_denoiser.hlsl`, `common.hlsl`, and `denoiser_compose.hlsl`.
This test deliberately does not subtract guessed fog, split fused radiance into
invented lobes, or blend raw color back into the denoised result.
