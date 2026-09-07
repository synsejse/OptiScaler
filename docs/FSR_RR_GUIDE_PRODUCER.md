# Cyberpunk DLSS guide producer: authored NoV boost

Initial read-only investigation, 2026-09-07, followed by the explicitly authorized
live A/B test recorded below. That later test temporarily changed the game setting;
the initial producer analysis did not. No production shader conversion was changed
for this experiment. **Disabling the authored NoV boost did not fix the image defect.**

## New finding

Cyberpunk's own DLSS guide-generation shader modifies the viewing angle before
computing integrated specular reflectance. The corresponding engine setting is
`[DLSS] NoVBoostMode`, registered with default `-1`. The captured guides strongly
match the modified-angle calculation, including where the auxiliary layer is
entirely zero.

This is not a missing generic NVIDIA albedo decoder. NVIDIA describes specular
albedo as view-dependent average reflectivity and supplies a BRDF-integration
example in the [Streamline RR guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#421-specular-albedo-generation).
AMD's [SDK 2.2 sample](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.2.0/Samples/Denoisers/FidelityFX_Denoiser/dx12/shaders/trace_rays_denoiser.hlsl)
also uses integrated specular reflectance, but does not apply this game-specific
angle boost. Applying another BRDF, gamma decode, RGB normalization, or guessed
lighting split to already-integrated incoming guides is not justified.

The new discriminating hypothesis is narrower: a guide intentionally adjusted
for DLSS may be unsuitable as the **literal lighting divisor** used by our AMD
fused path. Disabling the adjustment in the producer can test this without
estimating missing material values from the final image.

## Authenticated local sources

These identities refer to the installed Cyberpunk 2.31 files. Addresses are
preferred image virtual addresses with image base `0x140000000`, not runtime
ASLR addresses. Proprietary binaries and complete disassemblies must remain
local and are not included in this repository.

| Source | Identity |
| --- | --- |
| `bin/x64/Cyberpunk2077.exe` | SHA256 `a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991` |
| `engine/staticshader_final.cache` | SHA256 `bff160947aba8df144200247adc39b44c26322360628d875f7c7218ad26c59ff` |
| Extracted `m_dlssConvertData` DXBC/DXIL container | Cache offset `0x6d3e8c`, 5824 bytes; SHA256 `a4bcbce1667fb6f3e130a18482e6088e2835d0028582a52e5a4668f7fae1b7ee` |
| Shader's embedded hash | `c109d56f9e858338a7d68bc3180db41e` |

The previously extracted local evidence is in
`/tmp/optiscaler-motion-producer.OuvgNo/`: `provenance.json`, `extract.py`,
`dlss_convert.dxil`, and `dlss_convert.ll`. The shader is a 16×16 compute pass
writing diffuse albedo to `u0`, specular albedo to `u1`, and normal/roughness to
`u2`. Its per-pass constant buffer is `cb6`, 32 bytes.

### Registration and upload are connected

- Initializer `0x1400e2060` passes the name at `0x14312b680` (`NoVBoostMode`),
  namespace at `0x14300c86c` (`DLSS`), and signed integer default `-1` to the
  registration function at `0x14113bacc`. The configuration object is at
  `0x1438137c0`.
- That function stores the supplied name/namespace/default in the object.
  The guide dispatch at `0x14037dee3` reads its value field at `0x1438137f0`
  and writes it to byte offset 20 in the 32-byte per-pass constants block.
- `0x14037df30`–`0x14037df3e` submits that block. Byte offset 20 is precisely
  `cb6` register 1, component Y, which controls the shader branch below.

Thus the connection is established by registration **and the value's upload**,
not merely by similarly named strings. Existing files under
`engine/config/base/` and `engine/config/platform/pc/` use normal INI sections;
there was no existing `NoVBoostMode` override when inspected. Registration alone
does not prove an added INI file was loaded: a new capture must verify the guide
changed before interpreting an A/B result.

## Shader calculation

Let `v = abs(dot(normal, toCamera))`. Before evaluating its rational integrated
BRDF approximation, the game optionally replaces it with:

```text
boost(v) = 0.8 * v^3 / (v^3 + (1-v)^3)
```

The shader enables this when `NoVBoostMode == 1`, or when the value is `-1`
and the decoded material class is zero. Value `0` takes the unmodified-angle
branch. The semantic name of material class zero has not been authenticated.

The ordinary material path decodes base color by squaring its stored channels,
constructs diffuse reflectance using `(1-metalness)`, and derives F0 by mixing
`0.04` with linear base color. It computes integrated specular reflectance from
F0, the selected angle, and squared perceptual roughness. The specular guide
written to the output is already integrated, not raw F0 or square-root encoded.
The guide's normal alpha retains perceptual roughness: this finding does not
justify squaring roughness again in OptiScaler.

Other authored branches exist: a material-class specular adjustment, an
optional extra specular texture contribution, and deliberate white/white guides.
They prevent treating every pixel as an ordinary dielectric. The observation
below uses an explicitly limited population rather than claiming a universal
inverse material model.

## Captured-data check

Capture: `dump-20260906-224211-859Z-54-324-1000000-48105`, normal pipeline with an
explicit denoiser reset. Reconstruct the viewing direction from captured depth
and camera matrices and evaluate the **actual extracted shader's** rational
BRDF terms with F0 `0.04`. Compare unmodified `v` against `boost(v)`; everything
else is unchanged.

Selection: valid surfaces, exclude white/white guides, specular RGB spread below
half an 8-bit step, diffuse RGB sum between `0.02` and `2.8`, roughness above
`0.04`, and `v > 0.05`. This selects 612,924 pixels. It is a dielectric-like
comparison population, not recovered material IDs or proof that every selected
pixel is a dielectric. Source specular guides are 8-bit UNORM.

| Predicted specular guide | Median absolute error | 95th percentile | Within half an 8-bit step | Within two steps |
| --- | ---: | ---: | ---: | ---: |
| Actual BRDF, unmodified angle | 0.0020133 | 0.3082203 | 48.85% | 70.96% |
| Actual BRDF, boosted angle | 0.0010777 | 0.0019494 | 95.24% | 98.46% |

Requiring all four captured `ColorBeforeParticles` channels to be exactly zero
leaves 174,945 pixels.
The boosted model matches within two steps for **99.70%**, versus **72.86%**
using the unmodified angle. This is an additional population check, **not** proof
that the shader's optional transparent-guide branch was inactive: the static
binding trace below establishes that its `t3` is a different resource. Remaining errors can include
material variants, captured normal/roughness precision, and reconstruction
differences. These are guide-prediction measurements, not perceptual-quality
scores or a guarantee that disabling the boost improves denoising.

The read-only reproducer is in ignored
`build-artifacts/fsrrr-autonomous-20260907/analyze_cp_specular_producer.py`.
It takes the capture directory and optional `--shader` disassembly path, prints
JSON, and never rewrites inputs. Its private shader dependency is intentionally
not bundled for distribution.

## Transparent-guide adaptation: a separate engine resource

The same shader has another gated path reading texture `t3` and exposure data.
For an eligible material and nonempty `t3`, it attenuates diffuse and specular
guides by `1-clamp(t3.alpha,0,0.7)`, adds `x/(1+x)` to specular with
`x=t3.rgb/(5*exposure)`, and jointly normalizes both guides if the largest
channel of their sum exceeds one.

The installed executable's registration, graph construction and resource binding
establish that **`t3` is not the resource passed as `ColorBeforeParticles`**.
These are separate optional allocations with distinct engine keys:

| Registered setting (namespace `Rendering`) | Config object / value address | Engine resource key | Authenticated consumer |
| --- | --- | --- | --- |
| `DLSSDTransparentGuide` | `0x1433106c8` / `0x1433106f8` | `0x8f1b6a8f` | Guide-conversion `t3` and its availability flag |
| `DLSSDSeparateParticleColor` | `0x143310690` / `0x1433106c0` | `0x2eb2c2ae` | Streamline `kBufferTypeColorBeforeParticles` (39) |

Both objects contain a static default of true. Their initializers are
`0x1400e2804` and `0x1400e27e4`, respectively. Name pointers are `0x14312bfa0`
and `0x14312bf48`; both namespace pointers resolve to `Rendering` at
`0x142bd1328`. These names label actual configuration objects, not an inferred
meaning assigned to anonymous resource keys.

The connecting code is:

1. Graph assembly at `0x141d435d4` and `0x141d435f1` reads those values,
   gated by bit `0x40` of the current mode. Builder `0x141d41ee0`, called at
   `0x141d43667`, stores transparent-guide availability at node offset `0x18`
   and separate-particle-color availability at offset `0x19`.
2. That node's vtable `0x14312e180` points to resource construction
   `0x14076ecc4`. Offset `0x18` enables allocation of key `0x8f1b6a8f` at
   `0x14076ef19`; offset `0x19` independently enables key `0x2eb2c2ae` at
   `0x14076ef42`. The main color allocation uses key `0x3d7e6258`
   (FNV-1a32 of `color`).
3. Guide dispatch resolves key `0x8f1b6a8f` at `0x14037dca8`–`0x14037dcbd`
   into `[rbp+0x198]`. At `0x14037dd28`, it copies that handle into the fourth
   entry of the four-SRV array submitted by `0x14037dd4c`, after `gbuffer0`,
   `gbuffer1` and `gbuffer2`. This is the `t3` consumed by the identified
   `m_dlssConvertData` shader. Its availability also sets byte 16 of `cb6`
   at `0x14037ded2`, enabling the shader's optional branch.
4. Key `0x2eb2c2ae` is instead resolved at `0x14037e124`–`0x14037e13d`,
   placed in a different argument at `0x14037e283`, and passed through
   RR helper `0x141d4fdc0`. At `0x141d5003b` that argument receives internal
   tag 14. Enum mapper `0x141d50e30` maps 14 to 39, matching
   [`sl::kBufferTypeColorBeforeParticles`](../external/streamline/sl_core_types.h)
   in the official header. The transparent-guide handle is not substituted
   for this argument.

This is an authored guide adaptation, not a documented packing format to undo.
The captures made so far do not include the actual `t3`, its per-pixel values,
or a dispatch-time record of `cb6[1].x`; therefore they cannot establish how
many pixels execute that branch. Statistics previously computed from captured
`ColorBeforeParticles` are **not statistics of `t3`** and must not be used to
infer that the transparent-guide branch is active or inactive. Neither resource
has been authenticated as a complete pre-fog scene-color snapshot.

Generic render-pass routing does select among main color and these two resources:
at `0x140775d4f`–`0x140775db1`, node bits select main color, key `0x2eb2c2ae`,
or key `0x8f1b6a8f`. This identifies a target-selection mechanism, not the full
set or chronological order of writers. A live one-frame capture is needed to
connect their actual writer draws and contents to the later RR inputs.

## Controlled test procedure

1. Preserve the existing game configuration and baseline capture. Add only
   `[DLSS] NoVBoostMode = 0` as a reversible game configuration experiment.
2. Restart and capture the same scene with normal bridge conversion. First
   verify captured guides now fit the unmodified-angle model; otherwise the
   setting was not successfully exercised.
3. Inspect the composed buffer before SR, including reflective panels and
   atmospheric backgrounds. Keep AMD tuning, HDR/exposure, resolution, all
   conversion toggles, and reset state fixed. Compare unchanged input guides
   and source color while allowing for stochastic rendering noise.
4. Restore the baseline setting and repeat if improvement appears significant.
   Even a successful result would be a game-specific interoperability finding,
   not authority to replace arbitrary applications' albedos.

### Live result: producer switch confirmed, visual defect remains

The controlled test was subsequently run with OptiScaler build `c3861b38` and the
same AMD RR 1.1 provider. The normal fused conversion remained enabled: input
division, AMD denoising, output multiplication and residual addition; no diagnostic
conversion options or bypasses were active. Both selected frames had valid denoiser
history, no reset, identical AMD settings, 1280×720 rendering, 2560×1440 output and
NGX pre-exposure 1.

| Capture | Frame | Recorded frame duration | Unmodified-angle fit | Boosted-angle fit |
| --- | ---: | ---: | ---: | ---: |
| `native-baseline-nov-auto-20260906-232614-591Z-1-324-1000000-14824` | 14824 | 15.91 ms | 47.66% | 95.48% |
| `native-nov-disabled-20260906-233303-432Z-1-324-1000000-10989` | 10989 | 16.14 ms | 96.91% | 47.68% |

The fit uses the same dielectric-like selection and half-an-8-bit-step tolerance
described above: 585,229 baseline pixels and 579,462 disabled-boost pixels. This
large reversal authenticates that `NoVBoostMode=0` reached the actual guide producer;
an ineffective override is not the explanation for the negative visual result.

The two runs are not identical stochastic frames or histories. Camera translation
was approximately 6.7 mm and its forward-direction difference approximately
0.05 degrees. For the comparison, the disabled-boost capture was reprojected onto
the baseline grid using depth and camera matrices. Adding the captured jitter
delta gave the best agreement with the unchanged diffuse guide among the three
tested registration conventions. Median sampling offset was `(0.117, -1.399)`
render pixels; 95.6% of compared valid surfaces agreed in projected depth within
3%. Disocclusions and inconsistent-depth pixels were excluded from the numerical
region measurements.

Raw scene color and recomposed color were viewed with one shared exposure and tone
curve across both runs. Fused albedo was displayed separately using linear-to-sRGB
conversion. **The unwanted building/material outlines remain clearly visible after
AMD denoising and recomposition with NoV boost disabled.** In the fixed far-building
region `(744,76)–(914,220)`, the scene input is smooth while the recomposed image
contains building detail closely following the fused guide in both runs. The
correlation of recomposed-luminance gradient magnitude with fused-albedo luminance
gradient magnitude was 0.836 in the baseline and 0.839 with the boost disabled.
These correlations are descriptive, not perceptual-quality scores or proof that
albedo alone causes the defect; the alternate image is also bilinearly resampled.

Therefore the boost is a genuine game-authored guide difference, **but not a
sufficient explanation or fix for this failure**. Do not ship its removal as a
solution, or compensate by inventing a generic inverse albedo transform.

The immutable captures retain their original hashes. The CPU-only analysis and
shared-exposure PNGs are in ignored local files:

- `build-artifacts/fsrrr-provider-contract/compare_nov_live.py`
- `build-artifacts/fsrrr-autonomous-20260907/nov-live-comparison/comparison.json`
- `build-artifacts/fsrrr-autonomous-20260907/nov-live-comparison/shared-exposure-raw-composed-fused-albedo.png`
- `build-artifacts/fsrrr-autonomous-20260907/nov-live-comparison/far-building-matched-roi.png`

The following frame of the baseline capture, 14825, recorded 589.30 ms after the
first capture's readback hitch and was deliberately not used as a normal-cadence
comparison frame.

If runtime binding evidence is still needed, capture the matched producer
dispatch's `cb6`, `t0`–`t6`, and `u0`–`u2` with exact view formats and offsets.
In particular, compare `t3` with the later NGX resource by identity and values.
The existing D3D12 root-state and descriptor tracking are useful foundations,
but need shader/PSO identification and dispatch-time capture; descriptor-table
addresses alone cannot identify these registers reliably.

## Exact fog-pass identity and a producer-side diagnostic

The same cache contains the following authenticated pixel-shader techniques.
Each record is identified by FNV-1a32 of its own name, not by assuming that the
previous technique starts with `m_`. That shortcut initially misidentified the
Mid/Low variants because unrelated NRD techniques are interleaved in the cache.
The corrected local parser and disassemblies are in
`/tmp/optiscaler-fog-producer.57tNt6/` and are not distributed.

| Technique suffix (`m_applyFogOverlay_…`) | Name FNV-1a32 / record offset | DXIL embedded hash |
| --- | --- | --- |
| High | `0xf8b1e3d0` / `0x183ce` | `691805878fa5cd13a5ba32b91b3e2bd0` |
| Mid | `0x9205656e` / `0xdd7e` | `06f10d06332b9424b809ecd09a8f135b` |
| Low | `0x14d13a6e` / `0x17929` | `4e962c310434f4241bb3013f1a9a2e9a` |
| Simple | `0x3e96592e` / `0x1f552` | `26186e989615f3383a676767264cdecf` |

The executable function `0x14061f9e0`–`0x14061fde3` selects these exact keys.
It reads hardware depth (engine key `0xdebf0c27`) and binds the existing main
color resource (`0x3d7e6258`) as its render target. High/Mid/Low read depth,
volume texture `t38`, environment texture `t68` and constant buffers; Simple
reads depth and constants. **These shaders do not sample the existing scene
color.** Their RGBA output contains a fog contribution and an attenuation-related
alpha. The cached blend descriptor is decoded below; the active runtime pipeline
still needs to be checked before relying on it for a production change.

Three real boolean settings, all with static default true, permit a reversible
producer-side diagnostic:

```ini
[Developer/FeatureToggles]
VolumetricFog = false
DistantFog = false
DistantVolFog = false
```

Their connection to rendering is authenticated rather than inferred from names:

| Setting | Configuration object / value | Feature-mask bit | Value read |
| --- | --- | ---: | --- |
| VolumetricFog | `0x1432f78f0` / `0x1432f7920` | `0x18` | `0x141d49ebd` |
| DistantFog | `0x1432f7960` / `0x1432f7990` | `0x1a` | `0x141d49e90` |
| DistantVolFog | `0x1432f7998` / `0x1432f79c8` | `0x22` | `0x141d49efe` |

The objects' namespace pointer `0x14300ba68` names
`Developer/FeatureToggles`; registration uses `0x140857738`. The feature builder
`0x141d49540` calls the actual bit-setting helper `0x141c9f8c0`. Caller
`0x1404e4ec0` copies its returned mask into the view's offset `0x17d0`.
The fog draw reads that exact field at `0x14061fa4d` and returns without drawing
at `0x14061fa6d` when bits `0x18` and `0x1a` are clear and the independent
simple-fog path is inactive.

This does **not** authenticate a universal "disable every fog effect" switch.
Simple fog has a separate gate: feature bit `0x17` or an invalid/empty view
rectangle, plus positive environment simple-fog density. That branch selects
the Simple technique at `0x14061fb2e`; otherwise the ordinary path selects
High/Mid/Low at `0x14061fcb0`–`0x14061fcc2`. The three diagnostic settings do
not themselves disable that independent simple-fog gate or every possible
material/particle atmospheric effect.

The experiment is useful only if a new capture confirms that the source scene
color's atmospheric contribution actually changed. Hold conversion and AMD
settings fixed, compare raw and composed buffers in the same scene, then restore
the settings. Removing fog would deliberately alter the game and is **not a
quality fix or a proposed shipped default**. A visual improvement would support
the full-scene-color versus surface-lighting hypothesis, not supply missing
fog/transmittance buffers or prove that fog is the only incompatibility.

If explicit layer capture is still necessary, the smallest useful next capture
is: identify the above graphics PSO; record its blend descriptor and target view;
copy that target immediately before and after the actual fog draw; and save the
bound depth, volume/environment inputs and constant buffers. Separately capture
the guide producer's actual `t3` and `cb6`. Preserve resource identity and draw
order through the later NGX capture. Before/after RGB alone cannot uniquely
recover both fog color and transmittance; obtaining those terms would require
capturing/replaying the exact producer output and validating its original blend
equation, not estimating a layer split from final color.

### No authenticated pre-fog option at the RR interface

The enum mapper `0x141d50e30` includes internal tag 13 mapping to Streamline
`kBufferTypeColorBeforeFog` (41). However, RR helper
`0x141d4fdc0`–`0x141d503f3` does not submit tag 13. Its optional color/layer
submissions are tag 14 (ColorBeforeParticles), tag 12 (ColorBeforeTransparency)
and tag 15 (SSS), at `0x141d50042`, `0x141d5010f` and `0x141d50138`.
All 16 direct call sites of the tagging helper `0x14290fda4` were checked;
none supplies internal tag 13. No existing registered DLSS setting was found
that adds a pre-fog resource to this submission path. This is a bounded result
for the inspected executable and path, not proof that no alternate path exists.
The generic enum's presence alone is not evidence that an INI option can expose
a pre-fog buffer.

### Cached blend state and faithful reconstruction

All four authenticated fog records have the same blend bytes at record offset
52: `01 00 00 01 0f 00 00 05 05 01 01`. The engine's
[`PSODescBlendModeDesc`](https://github.com/WolvenKit/WolvenKit/blob/main/WolvenKit.RED4/Types/Classes/PSODescBlendModeDesc.cs)
and [`PSODescRenderTarget`](https://github.com/WolvenKit/WolvenKit/blob/main/WolvenKit.RED4/Types/Classes/PSODescRenderTarget.cs)
schemas decode these as one render target, independent blending disabled,
alpha-to-coverage disabled, then RT0 blending enabled, RGBA writes, ADD for both
RGB and alpha, destination factors INV_SRC_ALPHA, and source factors ONE.

The installed executable independently corroborates the enum values:
`OP_Add=0` is registered at `0x14128d55b`–`0x14128d570`, `FAC_One=1` at
`0x14128d6fc`–`0x14128d710`, and `FAC_InvSrcAlpha=5` at
`0x14128d760`–`0x14128d774`. Its render-target property registration also
matches the schema order (blendEnable, writeMask, colorOp, alphaOp,
destFactor, destAlphaFactor, srcFactor, srcAlphaFactor). Thus the cached
RGB operator is:

```text
post = F + (1-opacity) * pre = F + T * pre
```

Here `F` and `opacity` are the actual fog pixel shader's RGB and alpha, not
quantities fitted from final scene color. A live PSO descriptor must confirm
that the cache state was not subsequently overridden.

Preserving only `post-pre` would retain extinction calculated from the original
noisy `pre`. If `D` is the denoised/recomposed pre-fog surface image:

```text
D + (post-pre) = F + T*D + (1-T)*(D-pre)
```

The final term is an unwanted difference correlated with the denoising change.
It vanishes for identity denoising, which does not validate this construction
for real denoising. Therefore a post-minus-pre residual is not a faithful final
fix for an attenuating fog pass.

The narrow observational path is established: the fog node calls fullscreen
helper `0x14020c954` at `0x14061fc4a` with the selected shader key. That helper
tail-calls `0x14020cc64`, obtains the thread-local render context through
`0x1401f405c`, and directly calls its command list's vtable slot `0x60`
(`DrawInstanced`) at `0x14020cca9`, with arguments `(3,1,0,0)`. A version/hash-
gated node scope can therefore be correlated with the existing D3D12 draw and
render-target hooks; first validate this with metadata only, without changing
the render stream.

After validating the active PSO, an observation-only copy of that PSO with
blending disabled can run the **unchanged original shader** against a private
float RGBA target with identical inputs, constants, viewport and draw arguments.
Because the shader does not read scene color, this exposes its authored `F` and
opacity without estimating a split. Keep the original game draw unchanged and
verify that `F + (1-opacity)*pre` reproduces its actual post-fog target within
the source format's rounding tolerance. Track any subsequent color writers
before RR as well: authenticating this one fog operator does not establish the
semantics of later particles or transparent surfaces.

### Live fog control: improvement survives brightening the darker signal

The subsequent controlled fog test used capture
`native-fog-disabled-20260906-234744-076Z-1-324-1000000-11284`, compared with the
normal-cadence baseline frame 14824 above. Its recorded frame duration was
15.38 ms versus 15.91 ms in the baseline. AMD settings, provider, resolution,
pre-exposure, normal fused conversion and valid-history/no-reset state matched.
The NoV override was no longer active: the fog-disabled capture's selected guide
population fits the boosted-angle calculation at **95.62%**, versus **47.68%**
for the unmodified angle. Thus the improvement cannot be attributed to retaining
the preceding NoV experiment.

The raw scene color genuinely changed: the broad atmospheric contribution was
removed and previously obscured noisy surface lighting became visible. After
depth/camera/jitter registration, guide-luminance correlations between captures
were 0.959 for diffuse, 0.938 for specular and 0.935 for fused albedo. These are
strongly similar material guides, not bit-identical ones; registration,
animation and independently sampled frames remain limitations. Approximately
95.5% of compared valid surfaces passed the 3% projected-depth agreement test.

The source and recomposed RGB were inspected at one shared exposure across runs.
To check whether darkness merely hid the imprint, each case was also brightened
using an exposure derived from **its raw input**, then that same exposure was
applied to both raw and recomposed images. The fixed far-building region used
exposures 4.58 with fog and 25.80 without fog; the right-building region used
7.42 and 30.45. Even in these brighter comparisons, the fog-disabled output
visibly denoises the existing surface lighting instead of reproducing the strong
material-pattern outlines over smooth haze seen in the baseline.

For a brightness-independent descriptive check, central-difference luminance
gradients were divided by the local 5×5 raw-input mean, with a small explicit
floor. Only consistent-depth surface pixels with valid immediate neighbors were
measured. Positive excess is `max(composedGradient - rawGradient, 0)` divided
by that same local mean; it is not an image-quality score.

| Fixed region | Fog | Mean normalized raw gradient | Mean normalized composed gradient | Mean positive excess | Excess-gradient/albedo-gradient correlation |
| --- | --- | ---: | ---: | ---: | ---: |
| Far building, 5,675 pixels | On | 0.00198 | 0.17465 | 0.17268 | 0.854 |
| Far building, 5,675 pixels | Off | 0.59001 | 0.20853 | 0.02684 | -0.025 |
| Right building, 62,276 pixels | On | 0.06765 | 0.13807 | 0.09839 | 0.267 |
| Right building, 62,276 pixels | Off | 0.62741 | 0.15676 | 0.02915 | 0.026 |

With fog enabled, the far raw input is nearly smooth but recomposition introduces
strong guide-correlated gradients. Without fog, raw surface lighting contains
substantial noise and the denoiser reduces its gradients. Normalized composed
gradients do **not** simply become negligible in the darker case: the important
change is their relationship to the input signal and material pattern. These
measurements and the equally brightened pairs argue against darkness alone
explaining the visual improvement.

This is strong evidence that atmospheric composition participates in this
scene's failure of the surface-lighting demodulation/filter/remodulation path.
It does not identify an exact faulty-pixel fraction, prove every remaining
effect correct, or establish that fog is the only incompatible contribution.
The diagnostic disables multiple related render features and the captures have
different stochastic samples/histories. Disabling fog is still **not a quality
fix**; a faithful integration must preserve its authored contribution.

The CPU-only reproducer is
`build-artifacts/fsrrr-provider-contract/compare_fog_live.py`. Original captures
are hash-preserved. Results and progress images are in ignored
`build-artifacts/fsrrr-autonomous-20260907/fog-live-comparison/`:

- `comparison.json`
- `shared-exposure-raw-composed-fused-albedo.png`
- `per-case-brightened-raw-composed-pairs.png`
- `far-building-per-case-brightened.png`
- `right-building-per-case-brightened.png`

### Authenticated live fog draw and RR routing

The opt-in provenance build `2d0acd9b` authenticated all four exact installed
fog pixel shaders and their common vertex shader. In the saved test scene the
active pass uses High fog, an RGBA16F target, one non-indexed fullscreen triangle,
no depth/stencil target, and ONE/INV_SRC_ALPHA/ADD blending for RGB and alpha.
This confirms the traced premultiplied operator at the live draw boundary.

The one-shot capture build `46f3a225` records native RGBA16F pre/post snapshots
and the unchanged shader's authored output into private RGBA32F storage with
blending disabled. The original draw runs once against the original target.
Its resource/view is retained at the original RTV binding, and restoration uses
an immutable private descriptor: a CPU RTV handle may be recycled after binding.
Capturing requires both explicit INI opt-ins and a separate request marker;
unsupported identity, shader, format or command-list state refuses capture.
This is diagnostic instrumentation, not a fog correction.

Executable tracing also resolves fog and ApplyDLSS through the same
view-namespaced `color` key, `0x3d7e6258`. Fog constructs it at
`0x14061face`; ApplyDLSS resolves it at `0x14037e25f` and passes it through
`0x14037e2b5` to the internal tag that maps to Streamline
`kBufferTypeScalingInputColor`. There is no explicit pre-fog replacement there.
That is engine-key identity, not proof of identical live resources or pixels.

Importantly, graph construction appends FogOverlay at `0x141d45814` and
ApplyDLSS at `0x141d465b9`, with potentially active planar reflections,
holograms, distortion/heat haze, clouds, transparency, screen effects and
underwater work between them. Node class getters and engine stage strings
authenticate these identities; construction order alone does not prove live
execution or GPU ordering. In particular, recomposing only `F + T*denoised(pre)`
at RR could omit legitimate later content.

The next association check must link the owned fog target and exact recording
generation to the RR input and compare native values at that boundary. Raw
pointer equality across independent dumps, feature-local frame counters, and
CPU timestamps are insufficient: textures persist across frames and worker
threads can record GPU-later command lists first. Endpoint identity/order is
reported separately from any claim that no intermediate writer changed color.

### Native authored-fog capture closes the blend equation

Build `0bb40f44` captured the live High/RGBA16F fog draw in
`fog-20260907-010043-666Z-324`. The capture contains native, unfiltered
`scene_before.rgba16f`, `scene_after.rgba16f`, and the unchanged shader's private
`authored_fog.rgba32f`. The game draw was not replaced. All inputs are finite and
the original files' hashes are preserved by the analysis.

The authored RGB is the expected smooth, blue-gray atmospheric contribution;
its alpha varies from 0.00986 to 1 (median approximately 0.4021). Both main scene
alphas are exactly 1 everywhere, so main alpha is not the required transmittance.

The fixed equation `F + (1-opacity)*pre` explains the observed post-fog image.
Using an explicit precision reference—authored RGBA rounded toward zero to FP16,
FP32 blend arithmetic, then an FP16 round-toward-zero result—predicts
**99.9917896%** of the 2,764,800 RGB components bit-exactly. The remaining 227
components differ by at most one native FP16 step (maximum absolute error
0.00024414; mean 4.2834e-9). No coefficients or layer values were fitted.

This is strong evidence for the captured blend's source/target precision, not a
universal hardware requirement. Microsoft's [blending precision specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#BlendingPrecision)
allows FP16-target blending at target precision or higher. A full-precision
source reference has small systematic rounding differences; it must not be
mistaken for evidence of another lighting layer.

The endpoint observer also confirms that fog and RR use the same owned main
resource. However, all eight observed RR endpoints use different command lists
from the captured fog draw. The first RR CPU callback even arrives before the
fog capture has finished recording. Thus these observations do **not** establish
same-frame GPU ordering or unchanged color between the passes. Submission
provenance and a paired RR-input capture are still required before replaying a
purported same-frame correction.

### Separate-particle-color is not the main haze artifact

The `DLSSDSeparateParticleColor` setting routes a selected category of transparent
draws to view key `0x2eb2c2ae`, which ApplyDLSS maps to Streamline's
`ColorBeforeParticles` tag. Static draw-selection tracing finds valid
category-5/class-3 draws that occur only in this auxiliary pass; the auxiliary
cannot be assumed to be a universal duplicate of main color or simply added to
it without its blend contract. The fullscreen FogNode itself writes main color,
not this auxiliary. In baseline frame 14824, auxiliary alpha has 12,992 distinct
FP16 values and main alpha is uniformly 1.

An engine-authored control set `[Rendering] DLSSDSeparateParticleColor=false`
temporarily, with default fog and NoV and unchanged RR conversion. Capture
`particles-in-main-20260907-20260907-004949-336Z-1-324-1000000-16004`
has no BeforeParticles resource and a normal 16.263 ms frame interval. All 43
checked static RR manifest settings match the baseline. Within that frame, the
raw far tower is nearly hidden by haze but recomposed color again contains harsh
material structure (luminance p10–p90 width 0.013465 to 0.034591). This rejects
missing BeforeParticles as a sufficient explanation for the principal artifact.

The control used AutoSave18 in a separate run; stochastic state, bias-mask
coverage and camera/jitter differ from baseline. It is not an exact cross-run
pixel subtraction or proof that every particle effect is handled correctly.
The temporary engine override was removed after capture.

### Positive pre-fog denoising control, with submission evidence

Build `28860425` captured fog `fog-20260907-012146-595Z-324` and two immediate,
nonqueued RR candidates. Independent queue-call intervals show separate,
nonoverlapping submissions on the same queue, fog first. Candidate frame 10122
is pixel-bit-identical to native post-fog RGB over 97.0419% of the image. Frame
10123 matches only 6.0930%; this next-frame control has broad stochastic changes.
The first candidate's remaining 2.9581% contains localized later color changes.
These results support a controlled isolated-boundary experiment, not an assertion
that no later writers exist or that arbitrary future frames can be paired this way.

Three actual AMD fresh-context replays used the first candidate's identical
guides, native ray-length alpha and settings:

1. Captured current radiance, with its original numerical residual.
2. CPU-rebuilt current radiance/residual, as an arithmetic control.
3. CPU-converted native pre-fog radiance, with its own numerical residual; only
   after denoising/remodulation is the captured authored fog operator applied.

The third arm removes the unwanted revealed-material outlines in the fixed far
and right-building regions. Both current-color controls reproduce the defect.
The pre-fog surface-only view retains genuinely denoised detail; the same authored
fog, not a fitted or increased haze layer, is restored. The corresponding
identity round trip agrees with native post-fog within three FP16 steps.

Each arm was repeated in another fresh context. The provider is not bit-
deterministic in this test. On 473,835 unchanged-source pixels after excluding
32 pixels around any observed later-color change, current-arm repeat MAEs are
approximately 0.00027–0.00028, comparable to the 0.00028–0.00030 difference
between the two arithmetic controls. Thus that difference cannot be attributed
solely to CPU arithmetic. The current-to-pre-fog change is approximately
0.00951 in both runs, about 34 times the repeat variation. On the fixed right
region, pre-fog-plus-fog luminance remains close to the raw source while the
current path increases contrast and material-correlated structure.

This is strong positive evidence for separating atmospheric composition from
surface denoising. It is **not a production fix**: late-color differences and
the denoiser's unknown spatial reach remain confounds, and two reset-frame runs
are not temporal validation. A general live correction should denoise before
the game's own fog and subsequent effects, rather than replacing their final
result using a guessed residual. Current NGX guides are generated later, so
previous-frame parameter pointers are not a safe shortcut.

The next opt-in observation reads only bounded current-context graph metadata
at the authenticated, explicitly requested fog draw. It reports available guide
handles and authored settings; it neither calls engine getters nor accesses GPU
contents. A handle's presence does not establish initialized pixels, resource
state, or permission to dispatch early. Shared-view constants, motion/depth and
ray-distance availability still require corresponding producer/consumer checks.

### Early live observation and bound-constant probe

Build `75c8dd2d` completed native capture `fog-20260907-014608-188Z-324`.
At the authenticated High fog draw the four material inputs resolved to fixed
handles 22470, 22469, 22467 and 22483. The current view extent was 1280×720,
namespace zero, graph selector 71/position 4653175; NoV mode was -1 and optional
extra-specular feature 0x35 was disabled. The read-only lookup used 36 bounded
memory reads / 338 bytes. This proves CPU lookup availability for that scope,
not resource initialization, GPU state or equality with later guide inputs.

The next capture includes the exact *bound* High-fog cb12 registers 21, 22, 23,
24 and 27: a private 5×1 RGBA32_UINT target receives raw uint4 loads using the
unchanged original vertex shader, root signature and root bindings. It reads
only constants the authenticated original High shader already accesses. The
probe restores the original PSO, frozen RTV and exact viewport/scissor arrays;
it does not call an engine constant uploader, inspect a guessed GPU address, or
use a global “latest camera” pointer. Its 80-byte companion uses the existing
capture submission/fence and is not mixed with the three native float layers.

Probe provenance v2 uses five compile-time fixed-register pixel shaders and
five disjoint one-pixel scissor columns. The original screen-position-indexed
v1 probe could repeat adjacent registers under inherited variable-rate shading;
its readback completion alone was not sufficient evidence. V2 leaves shading
rate untouched: each draw returns the same constant for every invocation, while
native scissor coverage selects its output column. This avoids requiring a new
VRS state tracker or changing the game's shading rate. See the official
[VRS coverage semantics](https://microsoft.github.io/DirectX-Specs/d3d/VariableRateShading.html).

Additional CPU metadata records the authored depth key, motion fallback/owner
precedence, specular-hit-distance owner, and view-derived camera candidates.
These candidates remain explicitly distinct from final NGX constants/reset.
No early denoiser dispatch is enabled by this instrumentation.

The v2 probe completed in live build `4209c1d6`, capture
`fog-20260907-044320-648Z-324`. All five native UINT register loads were
recorded; cb27 exactly contains float32 `[1280,720,1/1280,1/720]`. The direction
matrix has positive homogeneous W at the fixed 221 sample positions. This
validates a usable GPU observation, not the as-yet-uncaptured CPU matrix source.
The early depth, motion and specular-hit-distance native addresses match both
subsequent NGX candidates. The four material-buffer addresses were also present.
Only 6.27% / 5.72% of candidate RGB pixels match native post-fog color, so neither
candidate is accepted as the same frame. Address equality and submission order
must not be substituted for image/frame identity.

Static tracing identifies the bound ray-matrix recipe: current jittered inverse
native projection (view+0x1c0) times inverse native view (view+0x180), with the
entire inverse-view fourth row replaced by `[0,0,0,1]`. The source helper uses
separate float32 multiplies/adds, not fused multiply-add. The next CPU-only
observation records raw source rows for an independent 80-byte comparison.
Jitter-free projection is a different matrix: it must retain authored lens
offsets, not blindly zero all projection offsets. The source FOV at view+0x90
is in degrees; the earlier `fov_radians` diagnostic label was incorrect. The
production AMD dispatch still derives its FOV from projection coefficients,
so correcting the label does not change that dispatch.

Live build `582a149a`, capture `fog-20260907-050934-789Z-324`, verifies that
recipe against the actual bound GPU words: all 16 matrix components and all
four dimension/reciprocal components match **bit-for-bit**. The comparison uses
the current callback's raw view-owned source rows, the authenticated separate
float32 multiply/add order, and no fitted transform or previous-frame camera.
This result validates this captured producer recipe; it does not by itself
validate the camera-position binding, the early guide pixels, or a same-frame
join with either subsequent NGX capture. All six selected graph-backed inputs
also lie within their observed compiler reservation intervals and before the
original end event. Compiler reservation is not GPU initialization, resource
state, or full alias-lifetime proof.

The metadata also records borrowed native resource addresses from the exact
32,768-slot engine texture registry, bounded by the authenticated slot layout.
Positive reference counts and repeated reads are required to emit an address,
but they are not ownership or atomic-snapshot proofs. No native address is
dereferenced, passed to COM or used for a GPU operation. The graph resource
holder itself is non-owning; its resource-record pool has a separate lifetime.

The correction still needs dedicated graphics/compute state preservation before
an early compute insertion. A different command list cannot simply be inserted
in the middle of an existing recorded list. Graphics and compute roots are
independent, but changing descriptor heaps invalidates descriptor-table state;
both are relevant to a safe scoped insertion. See Microsoft's
[root-signature semantics](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-a-root-signature)
and [descriptor-heap semantics](https://learn.microsoft.com/en-us/windows/win32/direct3d12/setting-descriptor-heaps).

Signal coverage also remains limited: the replay skips exactly the captured
hardware-depth-zero pixels (20.3861% in frame 10122), preserving their pre-fog
RGB. The good sky result is therefore not proof of successful AMD sky denoising.
Primary emission has not been separated from valid-surface pre-fog RGB; a bright
pixel threshold would not establish that semantic and is not used as a fix.

### Early guide descriptor and frame-provenance requirements

The shader's `Texture2D<uint2>` declaration for t4 does not imply a native
`R32G32_UINT` texture. ApplyDLSS uses special binding helper RVA `0x774be0`
for graph key `0x61f178d4`, selecting the alternate CPU SRV stored at texture
registry `+0x2f210+(handle-1)*0xb0`; ordinary SRVs use `+0x2f208`.
The authenticated descriptor factory's relevant branch creates a stencil-plane
view: format tag 18 selects DXGI 47 (`X24_TYPELESS_G8_UINT`), and tag 19
selects DXGI 22 (`X32_TYPELESS_G8X24_UINT`). It uses mip zero, one mip,
plane one and component mapping `0x1688`. Format values and the plane field
are defined in Microsoft's [DXGI format enum](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)
and [2D SRV structure](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_tex2d_srv).
Selecting the exact resource with its default view would not preserve the
authored guide input. New metadata observes descriptor-source integers and raw
engine format flags; it does not dereference a CPU descriptor handle or claim
the actual descriptor contents are already authenticated.

An opt-in thread-local observer around the original `slEvaluateFeature` call
records its real token, feature, viewport and command-buffer address only while
that call is active. Synchronously nested NGX candidates can inherit this
metadata; asynchronous/out-of-scope callbacks remain unavailable. Nested calls
mask and restore the outer scope. The token is not a Fog-frame join, and the
raw SL command-buffer pointer is not claimed to be a canonical native identity.
Late/early routing must not depend on whichever CPU callback happens first.

The live virtual frame getter resolves to RVA `0x18ec810`, whose complete leaf
is `lea rax,[rcx+0x10]; ret` (`48 8d 41 10 c3`). ApplyDLSS reads DWORD
`returnedObject+0x1a0` and passes that explicit frame ID to `slGetNewFrameToken`.
The bounded early observation therefore reads `context[0]+0x1b0` only after
matching this exact live target and code, and repeats the route/value reads.
It never invokes the virtual function. This records the early CPU source value;
only a subsequent token observation can establish whether that value was used
by a particular SL evaluation. Repeated reads are not an atomic snapshot or GPU
execution guarantee.
