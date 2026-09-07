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

Live camera capture has twice matched all 20 consumed GPU constant words
bit-for-bit. That verifies the tested current camera recipe, not every guide's
contents, GPU readiness, or frame ownership. In another capture the early
explicit engine frame ID was 108866 but subsequent observed SL tokens were
108867 and 108868; those samples must not be promoted to the same frame.

Before altering scene color, the integration still needs actual current input
initialization/version/lifetime and GPU ordering evidence, correct restoration
of engine command state, current temporal resources, and a validated early
denoiser/late-SR handoff. Numeric graph intervals and a reused resource address
alone are not that proof. Private guide capture is an intermediate check.

No fitted haze, inferred diffuse/specular split, raw/denoised blend, history
reset trick, or permanent removal of the game's authored fog is proposed.
