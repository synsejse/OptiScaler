# Cyberpunk RR input-fidelity research — 2026-09-06

## Conclusion

The largest measured input changes are **lighting heuristics and specular-albedo sanitization**, not motion-X/Y compression. Keep AMD's native fused mode; do not restore a guessed diffuse/specular split. There is promising evidence that Cyberpunk already carries depth-motion information in the otherwise-unused Z channel, but its encoding is not an authenticated engine contract yet.

This is an input-fidelity investigation, **not proof of DLSSD-equivalent image quality**. Two benchmark runs completed. A subsequent benchmark reload produced an AMD GPU fault, so testing was stopped and the previous tested installation restored. That failure needs investigation before more automated reload tests.

## Test and evidence

- Research branch: `research/fsr-rr-fidelity`, based on fused-only `704ccc83`.
- Captured Windows build: `be887e883d9e1cab80939d2bdea5919a47388e71`, built through [GitHub Actions](https://github.com/synsejse/OptiScaler/actions/runs/34044140012). Five HLSL kernels compiled, C++ regression tests passed, and seventeen integration guards passed. The numerical-analysis module was skipped in CI without NumPy; all twenty-three Python tests passed locally with NumPy 2.5.2 after the analysis follow-up.
- Cyberpunk 2.31, Radeon RX 9060 XT, NixOS / GE-Proton 11-6. Output 2560×1440, input 1280×720, Performance scaling, RR enabled, RT lighting Ultra, path tracing and frame generation off. The benchmark's RTX 4090 identification is spoofed, not the physical GPU.
- Three consecutive-frame pairs: `current_indoor` frames 7290–7291, `current_outdoor` 9203–9204, and `reference_indoor` 6133–6134. Labels identify manually requested bursts, not frame-matched camera positions between configurations. The first pair was near the bar/exit portion; screenshots taken afterward show the alley. The reference pair was earlier inside the bar.
- Each frame contains nine incoming and ten intermediate/output textures: **114 texture dumps**, plus six manifests. Captured rectangles are 1280×720. All six passed size validation; GPU submission and completed readback are logged. No captured texture contains a nonfinite channel value.
- Captures are shader-visible RGBA32F values, not native texture bytes or screenshots. All four available channels are retained; integer-normalized formats are decoded by the GPU. Optional inputs are inventoried even when absent. See [capture method](FSR_RR_RESEARCH.md).
- Full local evidence: `/home/synse/Games/Heroic/cyberpunk-fsrrr-fidelity-backup-20260906.9DpP7I/evidence/`. `texture-captures/` contains approximately 1.6 GiB of data; `texture-analysis.json` contains the measurements; `capture-sha256.txt` contains checksums. Game textures are deliberately not uploaded to GitHub.

Reproduce the numerical report with:

```sh
python tests/analyze_fsrd_textures.py /path/to/evidence/texture-captures
```

The analyzer requires NumPy and deliberately rejects conventions other than the captured hardware-depth / linear-albedo / packed-roughness configuration. These expensive diagnostic runs must not be used to compare FPS. The reference outdoor capture was not obtained; a late request remained unconsumed on the results screen and was removed before the reload attempt.

## What Cyberpunk actually supplied

The following inventory was consistent in all six captured frames. Presence means a resource was passed at the intercepted NGX evaluation point, not that every pixel contains useful signal.

| Input | Actual format / contents | Current adapter use |
|---|---|---|
| Combined noisy color | RGBA16F | Fused lighting, after floor estimate and demodulation |
| Depth | R32 typeless, viewed as R32F; reversed hardware depth | Reconstructed to absolute linear R32F depth |
| Motion | RGBA16F; varying X, Y and Z, binary W | X/Y copied; Z replaced with camera-only linear-depth delta; W discarded |
| Normal + roughness | RGBA16F; normal XYZ, roughness A | Octahedral normal and roughness packed into RGB10A2 |
| Diffuse material albedo | RGBA8 UNORM, alpha always one | Sanitized, floored and quantized to RGB10A2 |
| Specular material albedo | RGBA8 UNORM, alpha always one | Also reduced where diffuse + specular exceeds one |
| Specular hit distance | R32F | Packed into FP16 radiance alpha |
| `ColorBeforeParticles` | RGBA16F, nonconstant RGB and alpha | Fetched by composition, but its TODO read does not affect the result |
| Current-color bias mask | R8 UNORM, sparse nonzero values | Bound by conversion, but not consumed by its shader |

Absent in these captures: separate roughness (correctly packed in normal alpha), diffuse hit distance, dedicated specular motion, dedicated `MotionVectors3D`, ray-direction resources, reflected albedo, transparency layer/opacity/motion, the separately queried before/after-transparency/fog/SSS/refraction/DOF resources, and exposure texture. Scalar pre-exposure was 1.0 throughout. No genuine separated diffuse/specular lighting was obtained through this interface.

NVIDIA's public RR guide defines standard motion as a **2D field** and permits specular hit distance plus camera matrices instead of dedicated reflection motion. It does not establish semantics for Cyberpunk's extra motion Z/W channels. Material albedos and roughness are linear, and normals may be world- or view-space. [NVIDIA RR integration guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md).

## Measured conversion changes

Unless stated otherwise, percentages use the converter's eligible surface mask, excluding far-plane/zero-albedo pixels. Lighting ratios additionally exclude input luminance at or below 1e-4. Normal errors exclude degenerate input normals. Percentiles below are per frame, not a pooled distribution or a temporal quality score.

| Measurement | Observed result | Interpretation |
|---|---|---|
| Motion X/Y input → intermediate | Exactly zero numerical error, all six frames | No extra X/Y quantization or compression in this path |
| Normal direction after oct/RGB10A2 packing | 95th-percentile error 0.120–0.122° | Small but real quantization; not lossless |
| Roughness | Maximum absolute error 0.000487 | Small ten-bit storage error |
| Diffuse albedo | Maximum absolute channel error 0.000978 | Mainly the nonzero floor and quantization |
| Specular albedo | In current-path samples, 1.56–1.86% of eligible pixels change by more than 0.01 in a channel; worst change approximately 0.999 | Much larger than a format-conversion rounding error |
| Specular hit distance | 99th-percentile relative error approximately 0.081–0.083% | FP32 → FP16 quantization; no values above the shader's 65500 clamp in these samples |
| Raw → demodulated → remodulated + preserved lighting, before denoising | 99th-percentile relative luminance error 0.067–0.074%; maximum below 0.097% | Fused arithmetic nearly round-trips these samples, but this says nothing about the denoiser's temporal behavior |

No sampled input color was negative or above 65500. This does **not** validate those clamps for every scene, exposure or game.

The specular change is caused primarily by the energy-sum correction, not RGB10A2 itself. In the current-path samples, roughly 78–87% of the substantially changed pixels had **both diffuse and specular RGB equal to one**. They had finite surface depths and normalized normals, so the converter does not skip them. This could be special engine encoding or sentinel behavior; it is not enough evidence to declare the input wrong or blindly strip the correction without checking those surfaces. The reference samples had a different scene composition and far fewer affected pixels; that is not an improvement caused by disabling the floor.

The normal-space check strongly supports Cyberpunk supplying world-space normals: median absolute alignment with reconstructed smooth-surface geometry was 0.9985–0.9996 for that hypothesis, versus 0.0908–0.1897 when treating them as view-space. Shading normals are not geometry normals, so this is corroboration, not a general cross-game proof. Jitter and handedness conventions were not changed; the pinned SDK 2.2 sample remains the reference, not the conflicting old jitter comment or the changed RR 1.2 API.

## Lighting heuristics are materially changing the pipeline

Current path (`FloorIsolation=1`, `CorrelationBias=1`):

- The median per-pixel preserved-luminance fraction was **52.1–52.4%** in the first pair and **18.8–19.0%** in the outdoor pair.
- Weighted by total input luminance, the preserved component was **21.1–21.2%** and **13.8–13.9%**, respectively. This is estimated lighting bypassing the denoiser, not a measured separation of raster, emissive or particle light.
- Final correlation composition differed from pure denoised-remodulated-plus-preserved lighting by more than 10% of input luminance at **18.5–25.8%** of eligible lit pixels. It blends toward raw brightness using denoised chroma; it is not simply forwarding the denoiser result.

Reference (`FloorIsolation=0`, `CorrelationBias=0`), tested in a fresh game process:

- The median preserved fraction was zero. Only tiny floating-point residuals remained in these samples; no large FP16 overflow residual was exercised.
- No eligible lit pixels crossed that 10% composition-change threshold. The remaining composition difference was consistent with finite-precision storage/arithmetic; its 99th-percentile relative luminance difference was 0.29–0.32%.
- The full benchmark completed and sampled images remained recognizable/intact. This is **not** a matched temporal comparison proving the reference looks better. Removing a heuristic can also expose noise or blur previously hidden by it.

The supplied `ColorBeforeParticles` is not established as a clean additive-lighting separation. Its RGB differed from the main input at every sampled pixel, and subtracting it produced negative RGB residuals in approximately 0.75–31% of channels, depending on the scene. Its alpha also contains information. Check the resource's producer, exposure, lifetime and blend convention before using it; do not clamp that difference and call it a recovered particle signal. The bias mask was nonzero at roughly 0.004–0.12% of pixels and is another real guide worth investigating, not a diffuse/specular split.

## Extra motion channel: useful lead, not ready for production

In the captured Z values, a strong empirical hypothesis is:

`motion.z ≈ 1000 * (previousHardwareDepth - currentHardwareDepth)`

For pixels with W=0 and linear depth between 0.1 and 100, median error against the camera-only prediction was approximately **9e-6 to 1.22e-4 in encoded-Z units**. That is a much closer match than treating Z directly as AMD's linear-depth delta. W was exactly zero or one in every sample; its semantic meaning has not been confirmed.

The numerical analyzer also compares consecutive depth images reprojected using X/Y. Decoding the candidate Z representation gives a plausible previous linear depth. On the reference pair's W=1 subset, median disagreement with nearest reprojected previous depth was about **0.000315**, versus **0.000975** for camera-only depth. On the other pairs the difference was small. These comparisons have no independent object correspondence, use nearest-depth sampling without jitter correction, and retain disocclusions. Capture stalls also affect frame spacing. They are supporting evidence, **not validation of object-Z motion** or permission to hardcode an inferred scale for every game.

A verified encoding could let us convert actual engine depth motion into AMD's required units without guessing object movement. The next decisive evidence would be the shader writing this texture, or controlled static-camera/moving-object captures with an independent reference. Do not copy Z as-is, assume W means “dynamic,” or replace it with a guessed constant.

## Stability and next work

The normal-path benchmark and fresh-process reference benchmark completed. Reloading the benchmark again in that reference process then produced an AMD VM protection fault / graphics-ring timeout, a successful ring reset, and `VK_ERROR_DEVICE_LOST`. The last capture had completed several minutes earlier; there was no pending request. The diagnostic build was still loaded, so this does not rule out instrumentation effects or establish the root cause. Kernel and OptiScaler logs are preserved. No further GPU runs were attempted.

Recommended order:

1. Investigate resource lifetime / history / context recreation on benchmark reload with an uncaptured control. Avoid repeated GPU resets.
2. Establish a quality-first, matched-scene temporal comparison of pure fused denoising against the floor/correlation path. Keep estimated signal separation out of the reference; do not assume removing it automatically improves every scene.
3. Audit the specular-albedo energy correction on the identified white/special surfaces. Preserve legitimate material guides and separate any mathematically necessary demodulation denominator floor from rewriting those guides.
4. Verify Cyberpunk's extra motion encoding, then implement a game-scoped unit conversion only if confirmed. Investigate real particle/bias guides separately.
5. Consider wider AMD-compatible formats after testing provider support. Their small measured errors make them lower priority than the semantic issues, though a high-precision reference is still valuable.

AMD's native fused contract remains preferable to inventing missing diffuse/specular radiance. The branch targets the bundled **SDK 2.2 / RR 1.1** [interface header](../OptiScaler/include/fsr-rr/ffx_denoiser.h), with separate material guides and specular hit distance preserved. Different denoiser models and missing engine information mean that even a faithful adapter cannot promise identical DLSSD output.

## Installation restored

The pre-research `704ccc83` proxy and INI were restored. Proxy SHA-256: `a3ff0dbc735ec7892395ec820c9a6a2500674a38103f8cd2ce54402bf229ee1c`. All 102 original game DLLs, Heroic game configuration and Cyberpunk settings still compare unchanged with the fresh backup. The game, research-launched Heroic and test input daemon are stopped; T3 Code/Codex and unrelated work were not stopped. The diagnostic source and analysis remain isolated on the research branch; no production rendering changes were made by this run.
