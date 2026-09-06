# DLSS RR → FSR RR contract audit

Research update: 2026-09-07. Branch `research/fsr-rr-fidelity`, inspected commit
`cca2653593f82a68f396ad5a5eb3a4d084f5263b`.

This compares NVIDIA **ray reconstruction (DLSSD, not DLSS frame generation)**
with the **RR 1.1 provider from AMD FidelityFX SDK v2.2.0** and our current
Cyberpunk bridge. It is a research report, not a claim that the visual defect is
fixed. No production code, game DLLs, settings, or running game were changed.

## Result

Old temporal history is **not required** to produce the normal pipeline's
material-like outlines. The latest live reset capture and fresh-context offline
replays both reproduce them. History may affect their strength, but clearing it
does not address the main defect.

The audit found no additional justified gamma, roughness, BRDF, normal, camera,
or depth conversion for these Cyberpunk inputs, and no descriptor-layout error.
The principal unresolved compatibility question is still **what lighting is
being filtered**, rather than how its texture is encoded.

A newly found frame-time sample/header disagreement was tested below; it did
not remove the reset-frame outlines. A separate concrete finding is that AMD
provides internal diagnostic visualizations that our integration has not yet
exposed.

## Evidence from the new captures

The selected captures contain finite texture values, pre-exposure `1`, opaque
input color alpha, and a zero SSS guide:

| Frame | Active diagnostic options | Observed reset-frame result |
| --- | --- | --- |
| 47093 | `6`: skip albedo divide and multiply | Much smoother than normal; not a faithful normal-path test. |
| 47717 | `6`: skip albedo divide and multiply | Same distinction; cannot generalize this preset's history behavior to normal mode. |
| 48105 | `0`: normal signal conversion and composition | Material/detail contrast is already conspicuous on the explicitly reset frame, before SR. |

Their metadata records `diagnostic_manual_reset=true` and `denoiser_reset=true`.
The reset is RR-only; it does not itself reset the separate SR stage. Crucially,
the defect is present in the captured composition **before SR**, so retaining SR
history does not explain this observation.

Derived local evidence:

- [Live reset comparison](../build-artifacts/fsrrr-contract-20260907/live-reset-comparison.png).
- [Capture analysis](../build-artifacts/fsrrr-contract-20260907/live-reset-analysis.json).
- [Capture inventory](../build-artifacts/fsrrr-contract-20260907/capture_inventory.json).

These are diagnostic previews of linear buffers, not game-tone-mapped visual
quality comparisons. The original captures were not modified. Derived files
live in ignored `build-artifacts/` and are not included in a fresh clone.

## Input/output crosswalk

NVIDIA's published [RR integration guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS-RR%20Integration%20Guide.pdf)
and [Streamline RR guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md)
were checked alongside the [actual Streamline parameter forwarding](https://github.com/NVIDIA-RTX/Streamline/blob/main/source/plugins/sl.dlss_d/dlss_dEntry.cpp).
AMD references are pinned to v2.2.0, not a newer RR contract.

| Area | Current bridge and audit conclusion |
| --- | --- |
| Scene color | Divides complete NGX RGB by a fused albedo. Whether this is the surface-lighting signal expected by AMD remains unproven; see below. |
| Diffuse/specular guides | Preserves linear reflectances, with RGB10 quantization and `NON_GAMMA_ALBEDO`. No extra Fresnel/BRDF evaluation is justified. |
| Normal/roughness | Cyberpunk's world-space normals are oct-encoded; roughness is packed without an extra square. This matches the observed inputs and sample usage. |
| Depth | Converts hardware depth to absolute linear view depth. The captured conversion agrees with inverse-projection reconstruction. |
| Motion | Preserves/scales XY; uses the previously verified Cyberpunk-specific extra-channel decoding for linear-depth motion. No guessed generic object-depth motion is added. |
| Specular hit distance | Uses Cyberpunk's scalar distance in radiance alpha, bounded to finite FP16. Alternative combined direction/distance inputs are absent here. |
| Camera/jitter | Physical basis, previous-minus-current camera position, and projection signs match the actual sample conventions. Existing jitter conversion follows sample NDC values, despite pixel-unit comments. |
| Filtered output | Multiplies by fused albedo and adds the numerical residual, then invokes a separate SR stage. No missing output gamma decode was found. |
| Bias mask | Inherited FFX SR mask handling, not an AMD RR guide. It is sparse in this capture, not globally ignored. |
| Before-particles/SSS | Recorded for inspection; no authenticated layer decomposition. SSS is zero here. |
| Presets/settings | NVIDIA presets do not map one-to-one to AMD's six numerical controls. Our code queries the provider's defaults. |
| Descriptor ABI | Shared field ordering/types and relevant resource format/state enum values agree with the pinned SDK. The missing debug extension does not shift the main descriptor. |

Local implementation entry points:
[input preparation](../OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp),
[input conversion](../OptiScaler/shaders/fsrd_preprocess/precompile/FSRDInputConv.hlsl),
[composition](../OptiScaler/shaders/fsrd_preprocess/precompile/FSRDOutputComp.hlsl),
[validation](../OptiScaler/upscalers/ffx/FSRDInputValidation.h), and
[downstream SR](../OptiScaler/upscalers/ffx/FFXFeature_Dx12.cpp).

## Why fused lighting remains the important question

AMD's [native ray-generation sample](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Samples/Denoisers/FidelityFX_Denoiser/dx12/shaders/trace_rays_denoiser.hlsl#L261)
constructs integrated specular reflectance, diffuse reflectance, and a shared
denominator. In fused mode, the signal is schematically:

```text
A = max(0.001, diffuseAlbedo, specularAlbedo)  [per channel]
signal = (diffuseAlbedo * diffuseLighting
        + specularAlbedo * specularLighting) / A
```

Primary emission is retained separately. The [composition sample](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Samples/Denoisers/FidelityFX_Denoiser/dx12/shaders/denoiser_compose.hlsl#L153)
restores the filtered contribution's albedo and adds the skip signal.

Our bridge instead uses complete game scene RGB as the numerator on valid
surfaces. There, its skip signal is only the numerical remainder from
conversion/rounding/clamping. Invalid/sky pixels retain their complete original
color separately. This is **not** a separately identified atmosphere or emission
layer on valid surfaces.

The following is a possible mechanism, not a recovered Cyberpunk rendering
equation. If an input contains surface light `A*L` plus an additive contribution
`B`, dividing gives `L + B/A`. Filtering that mixture and multiplying by `A`
does not generally preserve `B`. It can introduce material-colored structure
even on the first frame. With an identity filter, the operations cancel, so
identity looking correct does not establish that the signal is appropriate for
the real denoiser.

That is consistent with the user's divide/filter toggle observations, but does
not prove that fog alone is responsible. Nor does it establish that albedo
remultiplication should simply be removed: AMD's native fused mode requires it.

`ColorBeforeParticles` differs from input color at every pixel in frame 48105.
Its name does not authenticate an additive lighting layer. NVIDIA distinguishes
before-transparency snapshots from a separate premultiplied transparency
color/opacity layer in its [RR guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS-RR%20Integration%20Guide.pdf).
Blindly subtracting this buffer or estimating missing diffuse/specular/fog terms
would violate the quality-first requirement.

## New frame-time discrepancy: tested, not adopted

AMD's [framework](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Kits/Cauldron2/dx12/framework/core/framework.cpp#L1625)
computes seconds and passes them to render modules. The [RR sample](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Samples/Denoisers/FidelityFX_Denoiser/dx12/denoiserrendermodule.cpp#L769)
assigns that value directly to `dispatchDesc.deltaTime`. The [API header](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Kits/FidelityFX/denoisers/include/ffx_denoiser.h)
documents milliseconds. Our bridge follows the documented milliseconds.

Controlled offline test: use normal-preset frame 48105, fresh reset contexts,
provider defaults, and identical captured inputs. Change only the dispatch
number from `16.6666667` to `0.0166667`; run each twice. The existing GH-built
replay executable and byte-identical installed AMD provider were used in a
separate Wine prefix. All four runs completed with finite output.

| Comparison | Mean absolute provider-output RGB difference over valid surfaces |
| --- | ---: |
| Milliseconds repeat 1 versus repeat 2 | 0.002076 |
| Seconds repeat 1 versus repeat 2 | 0.002059 |
| Milliseconds versus seconds, first runs | 0.002151 |
| Milliseconds versus seconds, second runs | 0.002023 |

Both choices retain the outlines. Cross-unit differences are similar in scale
to repeat variation; there is no demonstrated visual improvement. This is a
first-frame test, **not** a validation of temporal units or proof of zero effect.
No production unit change is justified by this result.

[Comparison image](../build-artifacts/fsrrr-contract-20260907/delta-units/comparison.png),
[measurements](../build-artifacts/fsrrr-contract-20260907/delta-units/comparison.json),
[hash provenance](../build-artifacts/fsrrr-contract-20260907/delta-units/provenance.json),
and [test script](../build-artifacts/fsrrr-contract-20260907/test_delta_units.py).

Replay/live parity is not exact: current dumps omit effective dispatch time and
the six current AMD tuning values. An INI without overrides cannot rule out
in-memory GUI changes. The units experiment isolates its own variable, not all
possible differences from the live dispatch.

## Additional checks and limits

Independent checks on frame 48105 found:

- Camera basis orthogonality error about `2.1e-7`; its determinant/sign and
  positive-Z projection agree with the sample's physical camera directions.
- Linear depth maximum relative error `1.37e-7` versus inverse projection.
- Converted normals: median angular error `0.060°`, maximum `0.238°`.
- Roughness maximum absolute error `0.000487`, consistent with quantization.
- Finite far plane `16777.236` is genuinely derived from Cyberpunk's projection.
  The sample's effectively infinite far plane is not evidence to replace it.
- No exposure texture; pre-exposure `1`; bias mask is one on `0.164388%` of pixels.
- No supplied separate fog/transparency, emission, alpha, refraction, DOF,
  diffuse-hit, reflection-motion, or ray-direction inputs in the audited dump.

Both vendors discuss minimally correlated noise and randomized reuse; that
shared expectation is not itself a vendor mismatch. See AMD's [integration
guidance](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Kits/FidelityFX/docs/techniques/denoising.md)
and NVIDIA's [guide, §3.5](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS-RR%20Integration%20Guide.pdf).

Exposure needs version caution: NVIDIA's published December 2025 guide §3.7
lists exposure/auto-exposure/sharpness as unsupported RR controls, while current
Streamline forwards exposure-related fields. Forwarding alone does not prove
their effect in every provider version. Current pre-exposure `1` does not point
to a missing exposure conversion as this capture's cause.

Separate portability gaps remain: view-space normals are not transformed;
missing-view/right-handed fallback deserves correction; some alternative ray
inputs and explicit transparency layers are unsupported. Nonzero consumed
subrect origins are explicitly rejected rather than silently misread. None of
these paths establishes the cause in the present Cyberpunk capture.

## Next useful work

Add the official AMD diagnostic output and complete dispatch metadata before
trying another signal modification.

SDK v2.2 includes an additive `ffxDispatchDescDenoiserDebugView` extension with
overview/fullscreen modes and up to twelve viewports. Our local header omits it,
and the release context currently does not enable debugging. This is a missing
diagnostic capability, not an ABI defect in normal dispatch. See the [debug
descriptor](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Kits/FidelityFX/denoisers/include/ffx_denoiser.h#L140).

Useful [documented views](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Kits/FidelityFX/docs/techniques/denoising.md#debug-view)
include Composed Luma, NN Input0/1/2, View Centered Pos, Virtual Hit Pos, and
reprojection UV/confidence. These are provider-generated **visualizations**, not
raw neural tensors or direct history-texture access. They can reveal how AMD
interprets our inputs, which the existing external-buffer views cannot show.

Capture those alongside effective `deltaTime`, all six tuning values, and reset
state. Keep the normal signal path unchanged. If this ultimately proves an
unavailable engine-layer requirement, the honest high-quality fix may require
engine-specific access to those layers, not a guessed image-space split.
