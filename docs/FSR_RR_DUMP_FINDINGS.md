# User buffer dumps: lighting and material-detail regression

Captured 2026-09-06, frames 8831 and 58531, installed source `9218fa28bae1311eff1ddf4f0a25eb08ce12aa61`.

**Latest finding:** the second capture reproduces strong specular-guide outlines on metal panels with zero diffuse albedo and an empty auxiliary RGBA layer. The investigation is broader than fog: see [second capture](#second-capture-reflective-metal-panels). The first-capture discussion below retains the atmospheric hypothesis as one possible contributor, not a proven sole cause.

## Result

The GUI dump works, and the captured frame reproduces the reported atmospheric/over-detailed appearance. The original game color has the expected haze. The denoised-and-recomposed color already has harsh, material-patterned distant buildings, **before temporal upscaling**. The final backend output retains the problem.

This establishes the failing processing stage, not a complete engine-level fix. The strongest measured explanation is inappropriate denoising of already-composited atmospheric lighting after surface-albedo demodulation. It is not evidence of motion-X/Y compression. One frame cannot validate temporal history or rule out other denoiser-input issues.

## Capture integrity

- GUI trigger, normal debug mode, neither denoiser nor upscaler bypassed, evaluation succeeded, `partial=false`.
- 21 texture files, 353,894,400 bytes (337.5 MiB). All file lengths match their manifests; every captured channel is finite.
- Inputs/intermediates: 1280×720. Backend output: 2560×1440, before OptiScaler sharpening, scaling and overlay.
- Running log identifies `9218fa2`; installed DLL SHA256: `299a6f3f9bf7906f6f4566818e7ee5b8f2802db7b5ec786fe1c099e034ed663e`.

Raw files remain unmodified at:

`/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/FSRRR-captures/dump-20260906-193025-079Z-1-324-1000000-8831/`

Local-only measurements and previews are in `build-artifacts/fsr-rr-capture-9218fa28/user-dump-8831/`:

- `input-vs-composed.png`: matching input/output views and exterior crops.
- `stages.png`: complete stage contact sheet, including the auxiliary layer and alpha.
- `analysis.json`: inventory, channel statistics and existing input-fidelity checks.
- `exterior_detail.json`: the fixed-region measurements below.

Previews use one common exposure multiplier (3.268296) and the same Reinhard/sRGB diagnostic display transform. They are not Cyberpunk's final tone mapping, separately auto-exposed comparisons, or reference DLSSD screenshots. Numerical measurements use original floating-point data, not these previews. Game images are kept local, not uploaded.

## Measurements

| Check | Measured result |
|---|---|
| Input versus converted motion X/Y | Exactly equal, maximum error 0 |
| Reconstruct original color before denoising | Maximum absolute RGB error 0.00001472 on valid surfaces; median 0.0000000277 |
| Normal conversion | Median angular difference 0.064°, maximum 0.238° |
| Exterior original versus composed log-RGB horizontal gradient RMS | 0.12824 → 0.39556 (3.08×) |
| Original-color gradient correlation with fused albedo | +0.1185 |
| Demodulated-input gradient correlation with fused albedo | −0.9599 |
| Denoised-radiance gradient correlation with fused albedo | −0.4915 |
| Recomposed-color gradient correlation with fused albedo | +0.8055 |

The fixed exterior ROI is `x=[200,940), y=[200,520)`, excluding pixels carried as invalid-surface/sky by the skip signal. Gradient pairs also require both neighboring pixels to be valid and all albedo channels greater than 0.002: 619,794 RGB-channel pairs. Gradients are differences of natural-log RGB, clamped to 0.000001 only for the logarithm. This is a contrast diagnostic, not a perceptual-quality score or ground-truth estimate of the denoised image. Raw input still contains noise.

Composition is consistent with remodulation plus residual at approximately FP16 precision (maximum absolute deviation 0.003803 from the full-float expression on valid surfaces), not a bit-exact float32 result. This small arithmetic discrepancy does not establish temporal correctness and must not be conflated with the much larger visual change from filtering the signal.

## Why the current mapping is problematic

For illustration, if a fogged pixel is `C = T × S + F` (surface color `S`, transmittance `T`, atmospheric contribution `F`), the current converter sends `C/A = T × S/A + F/A`, where `A` is surface albedo. The atmospheric contribution now carries the inverse of the material texture. Without denoising, multiplying by `A` restores the input almost exactly, which is why earlier round-trip tests passed. Filtering changes that inverse-material detail. Multiplication by `A` afterward can imprint material patterns onto what should remain smooth atmosphere.

The observed change from strongly inverse-albedo-correlated input to strongly positive-albedo-correlated recomposed color supports this mechanism in this scene. The equation is an explanatory model, **not a claim that this capture supplies measured `T` and `F`** or that they can be uniquely recovered from combined color.

The downloaded AMD SDK 2.2 sample constructs its fused input from genuine diffuse/specular surface lighting and keeps primary emission separately in its skip target. See local `trace_rays_denoiser.hlsl` around lines 263–280 and `denoiser_compose.hlsl`, under `/tmp/optiscaler-rr-sdk-review.moJo7G/source/Samples/Denoisers/FidelityFX_Denoiser/dx12/shaders/`. Sample archives came from the [official SDK 2.2 release](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.2.0). That is not the same input construction as demodulating Cyberpunk's already-composited color.

## What the auxiliary inputs do and do not establish

- `ColorBeforeParticles` is present. Its RGB/alpha preview depicts a smoke-like translucent layer, not a complete scene-color snapshot. Alpha spans 0–0.98877.
- 12.86% of the selected exterior pixels have exactly zero auxiliary alpha; those pixels still show large changes after denoising/composition. This layer alone cannot be assumed to describe all atmosphere affecting the scene. At 12 pixels across the full frame, zero alpha accompanies nonzero RGB; do not silently classify zero-alpha pixels as necessarily having no RGB contribution.
- Original RGB minus auxiliary RGB has a minimum of −0.0061035. Blind subtraction and clamping is not a proven separation. Producer identity, exposure and the actual blend state remain unverified.
- The newly inventoried SSS guide is present but all zero in this frame.
- Before/after fog resources, before/after transparency resources, explicit transparency layer/opacity, and emissive resource are absent at this NGX evaluation point.
- NVIDIA documents distinct semantics for a before-transparency scene snapshot versus a premultiplied transparency overlay; a parameter name or visual resemblance is not sufficient to authenticate this game's auxiliary resource. See [NVIDIA RR guide, sections 4.1.10–4.1.12](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md).

## Follow-up: oversharpening is not established as fog-only

The user also observes severe over-detailed distant surfaces where no fog is visible. Rechecking the existing exterior ROI with **all four auxiliary channels exactly zero** leaves 26,835 pixels and 25,672 valid horizontal neighbor pairs. Original versus recomposed log-RGB gradient RMS is 0.12114 → 0.44501 (3.67×); correlation with albedo rises from +0.1747 to +0.7433. This confirms the material-pattern amplification is not confined to pixels containing the captured auxiliary layer. Zero auxiliary RGBA is not proof that the engine applied no other atmosphere there.

The recorded `composed_color` precedes both the FFX upscaler dispatch (including its optional sharpening) and OptiScaler's later RCAS pass. Thus these sharpening stages cannot be the origin of the artifact already visible in that buffer, though they may increase its final visibility. Current evidence localizes the failure to denoising/remodulation; it does **not** isolate fog as the sole cause or prove every denoiser guide/history setting correct.

## Next correctness step

Broaden the investigation before implementing fog separation: obtain a normal-mode capture at a visibly fog-free failing location, check agreement/alignment between color and material guides, and compare reset versus accumulated denoiser history. An identity-denoiser control must retain the same conversion/composition/SR path (the existing `DenoiserBypass` skips conversion/composition too). Earlier mathematical round trips are useful, but not a GPU temporal-history control. Use these checks to distinguish signal/guide mismatch from history/reprojection and actual atmospheric-layer handling.

Trace the actual producers and blend state for Cyberpunk's atmospheric/transparency contributions, and establish whether pre-atmosphere surface color plus the actual atmospheric composition terms can be captured. If available, preserve/reapply those real contributions outside surface denoising. If they are not recoverable at the generic RR interface, a game-specific integration hook may be necessary; combined color plus material guides alone does not uniquely determine the separation.

Do not introduce an estimated fog floor, albedo-derived lighting split, brightness matching, raw/denoised blend, or arbitrary auxiliary subtraction as a supposed fidelity fix. No renderer changes, game termination, configuration changes or new DLL installation were performed during this analysis. The game was left running.

Reproduce the local diagnostic with the existing NumPy/Pillow environment:

```sh
/tmp/optiscaler-fidelity-python/bin/python \
  build-artifacts/fsr-rr-capture-9218fa28/analyze_user_dump.py \
  '/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/FSRRR-captures/dump-20260906-193025-079Z-1-324-1000000-8831' \
  build-artifacts/fsr-rr-capture-9218fa28/user-dump-8831
```

## Second capture: reflective metal panels

The user supplied frame **58531**, `dump-20260906-195626-007Z-2-324-1000000-58531`, from the same running feature. It is a separate viewpoint, not a consecutive temporal pair with frame 8831. The manifest again reports normal rendering, evaluation success, no reset and no partial capture: 21 textures, 337.5 MiB, all sizes valid and all channels finite.

The original image shows smooth dark metal panels across the opening. The recomposed color shows pronounced panel/rib outlines, retained in the final backend output. The auxiliary RGB/alpha layer contains some smoke, but is exactly zero over 82.44% of the entire frame. This is not proof that every atmospheric contribution is absent.

Local previews and measurements: `build-artifacts/fsr-rr-capture-9218fa28/user-dump-58531/`. `input-vs-composed.png` uses the same transform/exposure on both sides (multiplier 3.414834); `stages.png` includes the guides. `analysis.json` and `panel_detail.json` preserve numerical results. The older script's `exterior_detail.json` uses the first scene's rectangle; its broad-region figures must not be described as a matched material-region comparison across scenes.

### Useful isolation from this frame

Select rectangle `x=[330,650), y=[190,420)`, valid surfaces, **zero diffuse-albedo RGB**, and **zero auxiliary RGBA**. This yields 12,348 pixels on reflective panels at linear depth 36.71–41.74. All their converted fused-albedo channels exactly equal the converted specular-albedo channels; no diffuse/specular mixing weight is involved there. Median roughness is 0.3313.

| Selected metal subset | Before denoising | After denoising + recomposition |
|---|---:|---:|
| Horizontal log-RGB gradient RMS | 0.09742 | 0.30568 |
| Gradient correlation with fused/specular albedo | +0.0762 | +0.8746 |
| Median linear luminance | 0.020244 | 0.018299 |
| 95th-percentile linear luminance | 0.022715 | 0.035340 |

The gradient measurements use 11,795 same-subset horizontal pairs and the same positive-albedo/logarithm guards as the first analysis. Demodulated input is strongly inverse-albedo-correlated (−0.9722), while denoised radiance is less so (−0.6945). Recomposition therefore visibly exposes specular-guide patterns that were weak in the incoming color. The 3.14× gradient increase applies **only to this selected subset**, not to the whole image. In the broader first-script rectangle, gradient RMS actually decreases (0.39375 → 0.31514), as removal of stochastic noise outweighs some new structural contrast. Neither ratio is a universal sharpness or image-quality score.

This confirms a broader denoising/remodulation problem. It does not prove the engine's specular guide is malformed, nor that changing its values would be justified. These pixels are especially useful because the fused path reduces to specular-albedo normalization; a guessed diffuse/specular split cannot address them.

### Checks and remaining uncertainties

- Motion X/Y remains exactly unchanged. Pre-denoiser reconstruction has maximum absolute RGB error 0.000003794 on valid surfaces, versus the much larger observed structural change.
- Composition differs from full-float `denoised × albedo + residual` by at most 0.0009751; finite-format arithmetic alone is not an explanation for the pronounced patterns.
- A uniform-translation diagnostic swept denoised radiance by ±2 pixels in quarter-pixel increments on the selected metal subset. **Zero shift** best matches current input by median absolute log-RGB error. This does not establish correct temporal history, but supplies no support for a blanket coordinate-offset fix.
- Conversion and denoiser dispatch both select linear/non-gamma albedo; the SDK 2.2 header explicitly applies that convention to fused albedo as well. Merely seeing a square-root path in the sample does not establish a missing gamma conversion in this configuration.
- GPU history was not captured before filtering. The manifest records `reset=false`, but does not contain a complete history or all live denoiser tuning values. Reset versus accumulated history still needs a controlled runtime test.
- Other log entries show evaluation errors during earlier debug-menu activity. They do not coincide with this successful capture; they are not evidence of an error-free entire session and were not diagnosed here.

The next useful control is an **identity denoiser with the actual conversion/composition/SR path retained**, followed by a separate **forced denoiser-history reset** comparison on the same stationary scene. These isolate whether the visible issue needs AMD filtering/history versus the rest of the bridge. Ordinary `DenoiserBypass` changes too many stages to answer this on its own. No new rendering patch, arbitrary guide change, fog subtraction or game restart was made during these measurements.

Reproduce the additional subset and translation measurements:

```sh
/tmp/optiscaler-fidelity-python/bin/python \
  build-artifacts/fsr-rr-capture-9218fa28/analyze_panel_dump.py \
  '/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/FSRRR-captures/dump-20260906-195626-007Z-2-324-1000000-58531' \
  build-artifacts/fsr-rr-capture-9218fa28/user-dump-58531
```
