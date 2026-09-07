# FSR RR: the observed fog-ordering failure

Status, 2026-09-07: a corrective ordering works in a captured-frame AMD GPU
replay. **The live game still uses the existing late denoiser.** The early
guide work is an opt-in diagnostic, not a completed rendering correction.

## Why identity denoising passes

For the authenticated Cyberpunk fog draw, let `C` be pre-fog scene color,
`F` the authored premultiplied fog RGB, and `T = 1 - opacity`. Its blend is:

```text
post = F + T * C
```

The captured main-color alpha is one, not `T`. The authored source must be
observed separately; `ColorBeforeParticles` is not a universal fog buffer.

Dividing a fogged scene by a surface albedo sends the fog contribution through
surface demodulation too. Filtering that signal and multiplying the material
albedo back can expose material-shaped contrast that the authored fog obscured.
An identity denoiser cancels the division/multiplication, so its good result
does not establish that a genuinely filtered result has the correct composition.
This is consistent with the artifact appearing even in the upscaler-bypass view
and immediately after a history reset.

Merely saving an additive late residual is also insufficient. If the surface
denoiser produces `D`, adding the original `post - C` gives:

```text
D + (post - C) = F + T * D + (1 - T) * (D - C)
```

The final term is unwanted, vanishes for identity, and changes with denoising.
At this isolated boundary the correct expression is `F + T * D`. A correction
added still later to the final scene is not generally equivalent: later
transparent, refractive, and nonlinear passes may have changed that scene.

## Measured evidence

The exact installed executable and shader-cache identities are recorded in
[the native guide investigation](FSR_RR_GUIDE_PRODUCER.md). Proprietary shader
bytes, disassemblies and captures remain local, outside version control.

- A native fog-layer capture reproduced the original blend to FP16 precision:
  99.9917896% of RGB components were bit-identical; the remaining components
  differed by at most one FP16 step. The tested model includes observed
  source/target rounding. This is a capture-specific result, not a universal
  promise about every D3D12 implementation.
- Three AMD GPU replay arms used captured-current input, an arithmetic rebuild
  of current input, and native pre-fog surface input. The third was remodulated
  and composed with the **same captured authored fog RGB and opacity**.
  The first two retained harsh outlines; the third removed them while still
  denoising visible surface detail.
- Each arm was repeated in a fresh context. The ordering difference was much
  larger than repeat variation. The local reports retain source hashes and
  show the selected comparison regions and exclusions.
- This replay is not a whole-scene or temporal guarantee. Some later color
  changes were excluded from the paired boundary comparison. Depth-zero sky
  was preserved through the existing bypass; it does not prove AMD sky
  denoising. Primary emission has not been independently separated.

Local evidence is under ignored `build-artifacts/fsrrr-autonomous-20260907/`,
especially `fog-three-arm-012146/analysis/`. The user-facing contact sheets in
ignored `progress/` are `fog-three-arm-20260907-012146-far-building.png`,
`fog-three-arm-20260907-012146-surface-control.png`, and
`fog-three-arm-20260907-012146-repeat-stability.png`. Their display transform
is shared within comparisons; they are not the game's final tone mapping.

## Faithful live path and remaining gates

```text
Current surface + current material/depth/motion guides
    -> early AMD denoising and surface remodulation
    -> original game fog draw
    -> original later effects
    -> late super-resolution only
```

The early guide prototype uses the exact runtime-authenticated game shader,
exact current camera arithmetic and copied original SRV descriptors. It writes
private outputs with native RGBA8 albedos and RGBA16F normal/roughness; it does
not ship the game's shader bytes or guess material properties. Its initial
pre-transparency mode explicitly excludes later transparent-guide adaptation.
An active extra-specular branch with unavailable input must be refused.

Live camera capture has three times matched all 20 consumed GPU constant words
bit-for-bit. That verifies the tested current camera recipe, not every guide's
contents, GPU readiness, or frame ownership. In another capture the early
explicit engine frame ID was 108866 but subsequent observed SL tokens were
108867 and 108868; those samples must not be promoted to the same frame.

Before altering scene color, the integration still needs actual current input
initialization/version/lifetime and GPU ordering evidence, correct restoration
of engine command state, current temporal resources, and a validated early
denoiser/late-SR handoff. Numeric graph intervals and a reused resource address
alone are not that proof. Private guide capture is an intermediate check.

The first live private-guide admission test observed all four original GBuffer
clears with matching frame/view/resource identities, but the clears and Fog
were recorded on different native command-list generations. It correctly
refused dispatch; this was not evidence of a broken game frame. A separate
one-shot `FSRRR-lighting-guides.request` diagnostic instead intercepts the exact
final original lighting draw, where the four selected material SRVs are in
current use. It records private guide generation **after** that original draw,
preserves both original color targets and the depth/stencil read-only binding,
and emits three native-format textures to `FSRRR-early-guide-captures/`.
This avoids claiming that earlier CPU callbacks establish GPU ordering. The
capture is still diagnostic: it neither denoises scene color nor proves that
depth, motion, hit distance and temporal scalars are ready for early RR.

The first lighting capture on `35ec0b4e` completed successfully on the actual
GPU. All three native guide textures passed coverage/finite-value checks, and
the original game continued rendering after its bindings were restored. Native
FP16 output rounding must be accounted for when checking the 0.04 roughness
floor. This verifies the tested private guide path, not exact late-guide parity
or a finished scene/temporal correction.

The same opt-in diagnostic can also capture the seven raw words from the
original lighting shader's exposure buffer. It requires the exact observed
lighting VS/PS and selector, current matching VS/PS t37 descriptors, an
authenticated structured-buffer factory route, and repeated source identity.
The private integer copy performs no normalization; its optional fourth file
is a 2x1 RGBA32_UINT texture with seven original words and one zero pad. A
refusal leaves the three-guide diagnostic available. The late CPU exposure
readback getter is not called. This is needed because the main lighting target
and secondary target do not apply identical final exposure scaling; neither a
guessed reciprocal nor late scalar is promoted into current GPU evidence.

No fitted haze, inferred diffuse/specular split, raw/denoised blend, history
reset trick, or permanent removal of the game's authored fog is proposed.

## Current integration boundary

The original final lighting draw is a useful place to generate owned material
guides, but its main-color output is **not established as the immediately-pre-fog
color** used in the positive replay. Other authored passes, including a
conditionally enabled SSS pass, can target the same main-color resource. The
eventual scene correction therefore belongs before the original fog draw,
using color at that boundary and correctly ordered current guide outputs.

The ray node has the actual current motion and hit-distance uses; lighting's
pixel register t4 is a different input. Neither a lingering descriptor nor a
positive reference count extends a graph resource's alias lifetime. Preserving
those ray inputs must happen while their actual original use remains valid,
with explicit read-state transitions and producer/consumer ordering.

A successful `685f4507` capture observed original ray CPU constants selecting
absolute hit distance and enabling hit writes. Ray and lighting had matching
native command-list/reset identities in that sample. This is promising bounded
evidence, not proof of the selected GPU shader, immutable GPU constants, or a
fog-stage handoff. The next observer records original t4/u0/u8 and b6 descriptor
correspondence at the actual ray dispatch.

That observer succeeded in the `68dedefb` live capture. The first original
dispatch had matching b6/t4/u0/u8 receipts, and its view/list/reset/CPU frame
source matched the lighting sample. The current authored motion-scale
properties were both 1. The adaptive ray work domain was flattened, so it
must not be mistaken for the native 1280x720 texture extent. A second dispatch
changed u0 and constants; the observer correctly refused to reuse its earlier
receipts. This is not a claim that the first dispatch is the final hit writer.

The optional next-stage raw copy inserts copy-only commands synchronously
after the admitted original dispatch, before its end-use cleanup. It uses
the authenticated engine state-request/flush helpers and restores hit UAV
state before returning. Only private native-format copies may cross to the
lighting capture, with an exact same-list/reset/view/CPU-source join. Original
scene color is untouched; copy completion does not certify signal units or
final-writer semantics.

`FSRDPrivateDenoise` is an independent, one-shot RESET helper with private
outputs and explicit parameters. It passed local failure/lifetime tests and a
Windows build, but is not yet invoked by the live host. RESET avoids borrowing
late history for this first diagnostic; it does not excuse missing current
motion, hit distance, camera, or resource ordering. No extra exposure or raw
ray ×64 decoding is applied to already-composed main color.
