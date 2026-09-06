# Live albedo-pipeline tests

Open **FSR-RR Advanced Settings → Albedo pipeline tests (session only)**.
These controls diagnose the material-outline problem; they are not quality fixes.
Changes apply together on the next evaluation, without recreating GPU resources.

## Presets

| Button | Divide by albedo | AMD filtering | Multiply by albedo | Add residual | Temporal SR |
|---|---|---|---|---|---|
| Normal / reset tests | On | On | On | On | On |
| Identity | On | Off | On | On | On |
| No albedo round-trip | Off | On | Off | On | On |
| Lighting only | On | On | Off | Off | On |
| Original color + SR | Entire conversion/denoising/composition chain bypassed | — | — | — | On |

The five checkboxes can be combined independently after choosing a preset.
For comparisons without temporal reconstruction, turn **Temporal upscaling** off:
the composed color is bilinearly enlarged instead. In particular, **Identity +
Temporal upscaling off** now works, including the existing `UpscalerBypass` debug
view. Other debug views suspend the switches and the GUI says so; a preset resumes
normal composition. The `UpscalerBypass` view forces SR off regardless of its switch.

Start in a stationary scene. Compare Normal with Identity, then Normal with only
multiply-back disabled, then No albedo round-trip. Repeat with SR off if needed.
After each change, let the image settle and press **Dump buffers**. Each timestamped
manifest records the requested/effective switches, identity/AMD execution and resets.
These are sequential live comparisons, not simultaneous independent denoiser histories.

## Interpretation and safeguards

- Disabling divide sends combined color instead of normalized lighting. All material
  guides, normals, motion, depth and hit distance remain unchanged. This deliberately
  violates the normal AMD input convention; looking better does not establish correctness.
- Disabling multiply with divide still on displays lighting in different units, often
  much brighter. The Lighting only preset also excludes the numerical remainder.
  Multiply-off diagnostics clamp at the FP16 storage limit (±65504) to avoid feeding
  infinities into SR; this does not change normal composition.
- The residual is rounding/overflow compensation, **not a recovered fog/emission layer**.
  It follows the input divide switch, not the output multiply switch. Removing the
  residual also removes sky/invalid-surface color retained through that channel.
- There is no fabricated lighting split, guide replacement, blending or exposure correction.
  Existing OptiScaler and game postprocessing still run; the dump's `composed_color`
  is the useful pre-SR/pre-RCAS check.
- Changing divide or returning from identity resets RR once. Changing composition
  resets SR once; resuming SR after bilinear/debug presentation resets stale SR history.
  Manual **Reset denoiser history + dump** still resets only RR unless another transition
  or a game-requested reset coincides with it.
- Presets/switches are locked during a capture or queued manual reset. They are not
  written to the INI and reset on feature recreation/game restart. Normal restores the
  test switches, not unrelated FSR tuning or sharpening settings.

Validation includes CPU stage arithmetic, all 1,024 switch-pair transitions in the
compiled policy test, source integration guards and the Windows shader/DLL build.
Actual appearance and live game toggling still require the user's visual pass.
