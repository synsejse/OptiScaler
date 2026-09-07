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

### Same-frame native RESET control (2026-09-07, 12:22 UTC capture)

The matched native captures at `122224` share explicit CPU frame88999, its
source object, view and full repeated camera metadata; all20 bound GPU Fog
camera words match the Lighting recipe. Both actual completion fences finished.
The replay uses the production GPU conversion/composition shaders, native
motion/hit/material/depth snapshots, the same AMD1.1 provider and all six
explicit settings. The only control change is pre-Fog versus post-Fog color.
All six converted guide outputs are bit-identical between those two GPU runs.

`progress/native-reset-order-20260907-122224.png` and its `-far-surface.png`
crop show the post-Fog control reproducing the material outlines, while pre-Fog
AMD followed by the captured original Fog retains the haze. The latter Fog
composition is an explicitly labelled CPU precision reference, not a live
correction. No fitted exposure, guessed two-signal split or extra Fog is applied
to the post-Fog control. This replaces the earlier imperfect late-frame pairing
as the stronger composition-order experiment. It remains a single-frame RESET
test, not a temporal quality guarantee.

### Opt-in live private RESET

With the authenticated Cyberpunk probe/capture enabled and an already initialized
late RR feature, `FSRRR-prefog-reset.request` can request **one private-output-only
RESET per process**. Do not combine it with other capture markers. Example for
the tested provider (enumeration still verifies it exists):

```json
{
  "mode": "private_reset_only",
  "provider_id": 8382887756413599744,
  "delta_source": "explicit_reset_control_not_captured_duration",
  "delta_ms": 16.667,
  "settings": {"1": 1, "2": 1, "3": 65504, "4": 50, "5": 0, "6": 0.01}
}
```

Five fixed private targets are allocated before the request becomes visible.
Original ray and final-lighting callbacks produce those exact targets; Fog's
worker may record its consumer first. Before native submission, a non-optional
gate verifies the complete same-frame producers and their native state
restoration, same-list/Reset ray→guide ordering, and same-queue producer→Fog
ordering. Unknown, reversed, overlapping-unreturned or duplicate dependencies
are refused. Once consumer commands exist, failure terminates only the
authenticated Cyberpunk diagnostic rather than submitting undefined reads.

Fog captures additionally contain `private_reset_radiance.rgba16f`,
`private_reset_denoised.rgba16f`, and `private_reset_composed.rgba16f`, under the
actual consumer completion fence. The original scene RGB/alpha and Fog draw
remain unchanged. The private context resets once with explicit experimental
duration; no late temporal history is borrowed. Continuous history and the
early-RR/late-SR handoff remain separate work before a live image correction.

The first `269a1f93` build was deliberately **not armed** after review found a
pre-Fog binding-restoration gap. Private compute switches descriptor heaps;
Cyberpunk's SDK re-entry restores heaps, roots and global tables but only marks
dynamic tables dirty. Resuming the already-prepared raw Fog draw also requires
the engine's authenticated dynamic resource/sampler table flush. The repair calls
that native flush with the unchanged current cache/context and graphics selector,
authenticates its hot/cold bodies before mutation, and verifies the original t0
range is clean and still names the captured source. Failure after mutation is
fatal to the diagnostic recording. Merely resetting the original PSO is
insufficient under the [D3D12 descriptor heap contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/setting-descriptor-heaps).
Earlier copy-only Fog/depth captures did not switch descriptor heaps, and the
post-lighting compute path resumed normal engine draw preparation later. This
distinction must be covered before accepting the private pre-Fog GPU result.

### Earlier exploratory controls

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
at the ray node's authenticated common pre-cleanup entry, after its original
dispatches and before its first end-use cleanup. It uses
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

## Independently fenced native-input replay

The `b62d51d7` live test saved all seven guide/ray/lighting inputs successfully.
Nearby Fog draws used a different native command list. Recording proximity is
not a dependency or a same-frame proof, so no cross-list guide reads are added.

The next diagnostic permits one lighting capture and one Fog capture per
process (256 MiB readback each, 512 MiB combined ceiling). Each retains its own
submission ticket and completes on its actual GPU fence. Fog optionally captures
the currently bound pixel-t0 hardware depth into a private native R32_FLOAT
texture, beside its existing exact pre/post color and authored fog snapshots.
No depth linearization, resampling, or shader arithmetic occurs during capture.
The authenticated native state helper handles the source; only the private
output gets an explicit transition. Original fog executes once, unchanged.

The Fog manifest records current source-object/frame/view/camera observations
and an explicitly unpaired lighting-recording candidate. Offline replay must
match these against the separately completed lighting manifest, not choose the
latest directory or reuse mutable original resources after fence completion.
This diagnostic is still not a real-time image correction or temporal-quality
validation. Its direct copy contract follows [D3D12 CopyTextureRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion).
