# FSR RR captured-frame replay

Diagnostic tool, not a game mod. Runs one fresh-context RESET dispatch through a
locally supplied AMD RR 1.1 provider, on an explicitly named adapter. It neither
starts the game nor installs DLLs. Captured game data is never uploaded to CI.

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

Compare both results locally (also requires Pillow):

```sh
python tools/fsrrr-replay/compare.py CAPTURE NEW-comparison NEW-linear-result NEW-sqrt-result
```

Do not replace any game or prefix files for this test. Required DXVK/VKD3D DLLs
may be copied alongside the replay executable when using Wine. Results are tightly
packed little-endian RGBA FP16, plus JSON recording the provider/default settings,
camera/dispatch data, source manifest and input hashes. Output paths must be new.

The baseline restores native texture formats from shader-visible values and
checks repacking error. Original allocation sizes are retained separately from the
active render rectangle; uncaptured texels outside it are zero-filled, not reconstructed.
Sqrt mode sqrt-encodes **all three albedos**, requantizes
to RGB10, and clears NON_GAMMA_ALBEDO, matching the SDK 2.2 sample's representation.
Radiance/depth/motion/normals do not change. The resulting small albedo quantization
difference is recorded; this is not an exactly equivalent mathematical input.

New captures with `amd_dispatch` and `amd_settings` replay the recorded camera,
jitter, frame duration and all six accepted provider settings. `--delta-ms` overrides
the recorded duration explicitly. Old manifests lack exact frame duration and live
tuning settings: for them the tool defaults to 16.667 ms, uses provider defaults,
and reconstructs camera delta from float32 matrices. The result records defaults
separately from the accepted `effective_settings`. A fresh context with RESET
is not guaranteed bit-identical to resetting an existing context. Compare the
linear replay with the captured reset result before drawing conclusions. A single
frame does not validate temporal quality or moving-object reprojection.

Reference: AMD FidelityFX SDK 2.2 `Samples/Denoisers/FidelityFX_Denoiser/shaders/`
`trace_rays_denoiser.hlsl`, `common.hlsl`, and `denoiser_compose.hlsl`.
This test deliberately does not subtract guessed fog, split fused radiance into
invented lobes, or blend raw color back into the denoised result.

## Native AMD diagnostic views

Request the optional provider visualization while preserving the ordinary denoised
output from the same dispatch:

```sh
python tools/fsrrr-replay/prepare.py CAPTURE NEW-debug-job --debug-view overview
python tools/fsrrr-replay/prepare.py CAPTURE NEW-fullscreen-job --debug-view fullscreen --debug-viewport 8
```

The optional schema-1 job entry is:

```json
"debug_view": {"mode": "fullscreen", "viewport_index": 8, "output_size": [1280, 720]}
```

Omit `debug_view` to retain the non-debug context. An empty object requests overview
at render size; valid viewport indices are 0–11. A request enables
`FFX_DENOISER_ENABLE_DEBUGGING`, adds a dedicated zero-initialized RGBA16F UAV, and
chains the SDK debug descriptor into the same RR dispatch. No inputs are replaced.
`denoised.rgba16f` remains the normal result. `debug.rgba16f` contains tightly packed
little-endian RGBA FP16; `result.json/debug_output` records its dimensions, bytes,
mode and viewport. Alpha marks pixels written by AMD; do not treat alpha-zero
overview borders as meaningful diagnostic values. These are provider-generated
**visualizations, not raw neural tensors or directly accessible history buffers**.
Enabling debug can affect performance/memory; compare non-debug and debug denoised
results before interpreting an apparent difference.

Optional `provider_settings` maps numeric strings `"1"`–`"6"` to finite FP32 values.
Only requested keys override queried defaults; provider rejection fails the job.
These keys are AMD's six configuration controls, not DLSS presets. The replay does
not assume that supplied settings are visually appropriate.

The Windows build runs CPU-only `options_tests.cpp` before compiling the replay.
Recorded-metadata tests can also run locally without loading a provider:

```sh
python -m unittest discover -s tools/fsrrr-replay -p 'test_*.py'
```

Pinned [AMD diagnostic API and sample](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Kits/FidelityFX/docs/techniques/denoising.md#debug-view).

## Native-input RESET diagnostic

The optional schema-1 `input_mode: "cyberpunk_native_reset_v1"` accepts seven
native snapshots: composed color, hardware depth, motion, world-normal/roughness,
hit distance, diffuse albedo and specular albedo. It runs the **same production
GPU input-conversion and output-composition shaders**, not a CPU approximation.
It requires exact current camera words, explicit motion scales, provider ID,
all six settings, and an explicitly labeled frame-duration experiment control.
It refuses a supplied converted dispatch or uncaptured allocation padding.

Raw input SHA256 values are checked before GPU work. The result includes the
eight native converter outputs, `identity_composed.rgba16f` and
`denoised_composed.rgba16f`, plus the effective dispatch and shader hashes.
Identity replaces only the denoised signal; both controls use the production
compositor. No fog equation is applied and no game resources are accessed.
All resources remain owned until the standalone command-list fence completes.

This mode does not establish frame pairing by itself. Preparation must compare
independently completed captures' explicit frame-source object/value, view and
camera data; file names, directory order and pointer survival are not evidence.
One fresh RESET result does not validate temporal quality. The raw camera/options
contract can also be checked without loading the provider or creating a device:

```text
fsrrr-replay.exe --validate-native-reset job.json
```
