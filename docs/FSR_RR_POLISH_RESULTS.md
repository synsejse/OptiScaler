# RR fidelity follow-up — 2026-09-06

This follows the [original capture research](FSR_RR_RESEARCH_RESULTS.md). The work is on `research/fsr-rr-fidelity`. Production changes, builds and runtime evidence are separated below; successful arithmetic tests alone do not establish temporal image quality or GPU stability.

## Correctness changes

- Removed the estimated raster-light floor, diffuse-plus-specular energy correction and raw-brightness correlation blend. AMD receives native fused lighting with separate, preserved material guides. No invented diffuse/specular lighting split was reintroduced.
- Demodulation uses the quantized fused denominator; explicit FP16 rounding precedes the signed numerical residual. Composition only remodulates and adds that residual. The pre-denoiser round trip retains finite negative source values and representable highlights; denoising itself can change them.
- Authenticated Cyberpunk's extra motion channels from the installed 2.31 shader producers, then implemented a game-scoped conversion of authored hardware-depth delta to AMD's historical linear-depth delta. Historical projection, reset and camera history are handled together. See [producer provenance, formula and limits](FSR_RR_MOTION.md).
- Replaced converter/compositor frame-count descriptor rings with bounded, actual-queue-fenced dispatch storage. Recorded constants, descriptors, textures and pipeline objects survive until completion. The shared SR reactive-mask pass received the same lifetime repair and no longer resets its state tracker when reusing a texture.
- Separated exact-render-size composition output from display-capacity conversion storage, corrected padded-color source-state restoration, and separated SR versus RR provider versions.
- Reject unsupported input subrects, motion layouts, late barrier configurations, invalid scalar data and ambiguous `R16_TYPELESS` non-hardware-depth inputs. These checks avoid silently inventing an interpretation.

The selected RGB10A2 guides/normals and FP16 signal formats match AMD SDK 2.2's sample. They are preferred sample formats, not a claim that the API requires these exact formats. Wider-format provider acceptance and internal precision remain unverified. This adapter is not lossless or equivalent to a native DLSSD integration.

## Builds and automated checks

All installed Windows DLLs were committed, pushed, built by the existing GitHub workflow and downloaded with `gh`. No production DLL was compiled locally.

| Source | Build | Scope |
|---|---|---|
| `8deda142` | [34047295357](https://github.com/synsejse/OptiScaler/actions/runs/34047295357) | Pure fused fidelity and converter lifetime |
| `8053ea57` | [34047778586](https://github.com/synsejse/OptiScaler/actions/runs/34047778586) | Verified engine depth motion and input/state fixes |
| `d398f06f` | [34048775280](https://github.com/synsejse/OptiScaler/actions/runs/34048775280) | Shared reactive-mask lifetime/state and ambiguous-format validation |

At `d398f06f`, 64 Python tests pass locally with NumPy. Portable C++ projection/depth-motion/render-size and submission-policy tests also pass. CI compiles the HLSL and C++; NumPy-dependent analysis tests are skipped on the Windows runner when NumPy is absent. Source-pattern guards are not substitutes for D3D12 validation or GPU execution tests.

All three builds succeeded. The final CI log reports 54 Python tests with the NumPy module skipped, plus both passing C++ executables. The downloaded `d398f06f` archive SHA256 is `5052c5fdb27dc5c8f6e116f682780fca16184bff51ae23ffbffb72cfbaa8092b`; its proxy DLL is `33a979325b4f85a638c052984fe473f78fe719ffbbca3517aa35d4e3f5f8ebac`.

## Post-fix capture findings

Build `8053ea57` captured frames 7859–7860 (label `polish_indoor`, but the texture previews show the street-level alley after the bar). Labels identify requested bursts, not reliable scene classifications or frame-matched positions against the old run. A second requested outdoor burst was too late, remained unconsumed on the results screen and was removed; only one pair is claimed.

Each frame has 19 shader-visible RGBA32F texture dumps at 1280×720. Readbacks completed and all channels are finite. These captures include the actual engine-depth-motion path.

| Check | Result |
|---|---|
| Motion X/Y | Exactly unchanged; maximum error zero |
| Diffuse/specular albedo | Maximum absolute channel difference about 0.000483; no changes over 0.01 |
| White/white guides | All 81/85 sampled pixels preserved exactly |
| Zero diffuse guide | All 30,064/30,055 sampled pixels preserved exactly |
| Guide sum above one | Approximately 25,000 pixels per frame retained without energy correction |
| Pre-denoiser fused round trip | Maximum absolute RGB error about 0.0000258 |
| Composition | Matches intended remodulation/residual within one FP16 ULP everywhere |
| Converted depth motion | Engine decoding valid for all eligible sampled pixels; difference from CPU decode/FP16 reference in linear-depth units: p99 at most 0.0000610, maximum 0.000488 (not error against ground truth) |
| W=0 historical-depth reprojection | Median disagreement about 0.0000183 with converted depth motion versus 0.000289 camera-only |

The last comparison is corroboration, not independent ground truth: texture reprojection, disocclusion, finite precision and capture stalls affect it. W=0 identifies the inspected camera writer, not a universal static/dynamic classification. The analyzer constructs camera-only motion independently, so it does not mistake the newly converted engine motion for the baseline.

Independent negative controls also support XY and jitter: supplied motion differs from camera-derived motion by 0.0196 pixels median on the W=0 subset, versus 7.49 pixels after negating X. A fixed-mask temporal-depth comparison has median error 0.0000219 with the current jitter correction, 0.00902 without it and 0.01844 with its sign reversed. These frames do not justify a global motion-vector flip or rescale.

Matched input/composed texture previews show effective denoising, with some sparse colored edge speckles worth later temporal comparison. A later screenshot contains apparent trails in a different camera position; this pair cannot attribute them. Numeric correctness is not a claim that every reflection, moving edge or disocclusion looks as good as DLSSD. No visual heuristic was added to conceal those limitations.

## Runtime isolation

Cyberpunk 2.31, RX 9060 XT, GE-Proton 11-6 / vkd3d-proton 3.1.0, 2560×1440 output and Performance input, RT Ultra, frame generation/path tracing off. The game's reported RTX 4090 is spoofed, not the physical rendering device.

- `8deda142`, RR on, capture off: completed benchmark (60.88 average FPS), then AMD GPU fault during return to main menu.
- Same build/settings, only RR disabled: completed benchmark (61.83 average FPS), returned to main menu successfully, no new kernel GPU fault.
- `8053ea57`, RR on, diagnostic capture/fault logging enabled: completed benchmark (60.22 average FPS), then reproduced the exit fault. **Do not compare this capture-run FPS as a clean performance measurement.**
- `d398f06f`, RR on, capture off: two consecutive Performance benchmarks completed in the same process (61.07 and 60.93 average FPS), and both returned to the main menu without a new kernel GPU fault. Fault logging remained enabled; this was not a capture run. The second run's menu displayed Quality, but its actual render dimensions and results remained Performance, so it is not counted as a resize test.
- Same `d398f06f` process, after a confirmed quality-setting change: a new RR feature initialized at **1706×960** (verified by both the log and overlay). The Quality benchmark completed at 42.76 average / 37.18 minimum / 49.61 maximum FPS, 2,747 frames in 64.25 seconds, then returned to the main menu successfully. This exercises feature recreation and history initialization at a different input resolution; it is not a test of every in-place dynamic-resolution path.

The final build therefore passed three benchmarks and three menu returns in one process, including Performance → Quality. No kernel GPU fault occurred during this session. The earlier exit failure did not recur after the shared reactive-mask lifetime/state repair. This is useful regression evidence, not proof of a single crash cause or exhaustive long-session stability. No RADV mapping/DCC workaround was applied. Existing provider-probe/Streamline startup messages remain in the logs; the success claim is not that every log entry is warning-free.

No RR recreation or converter storage expansion near the failing exits established descriptor-ring pressure as the cause. The converter and Bias fixes address real lifetime/state defects, but crash causation must be separately demonstrated.

`VKD3D_CONFIG=fault` produced an allocation history: the fault address was 98,304 bytes into a recently reused range with 1280×720 RG16_SNORM and R8_UNORM image bindings. The converter's output is not RG16_SNORM. A currently mapped CPU-side range does not prove old GPU accesses or aliases are safe; the log does not name the faulting command or establish a driver/DCC defect. The [pinned vkd3d address tracker](https://github.com/HansKristian-Work/vkd3d-proton/blob/c9c6bf2e9c18252dce304272e7ea47d524287b6c/libs/vkd3d/address_binding_tracker.c) explains the reported history. System coredump data was not readable under this process's permissions; no privilege or device-permission changes were made.

## Local evidence and recovery

Before the first override, all 102 original game DLLs compared unchanged against the original-DLL archive. A fresh recovery directory preserves that archive, the pre-research proxy/INI and game/launcher settings:

`/home/synse/Games/Heroic/cyberpunk-fsrrr-polish-20260906.HcOD4k`

`evidence/8deda142/`, `evidence/8deda142-rr-off/` and `evidence/8053ea57/` contain run logs and screenshots. The latter includes `texture-captures/`, `texture-analysis.json` and `fidelity-detail.json`. `evidence/d398f06f/` contains the final successful runs, the verified Quality overlay/results/menu screenshots, full OptiScaler/launcher logs and the kernel-log check. Proprietary shaders and game texture dumps are kept local, not uploaded to GitHub.

## Final disposition

- The tested `d398f06f` proxy remains installed as `bin/x64/dxgi.dll`; its SHA256 matches the downloaded GitHub artifact. Subsequent report-only commits do not change that binary.
- All 102 original game DLLs still compare unchanged against the original-DLL archive. The proxy/INI and original-file backups remain available in the recovery directory above.
- Game settings were restored to the saved pre-test settings, including Performance mode and difficulty. The original OptiScaler INI is restored, capture is disabled, and the per-game `VKD3D_CONFIG=fault` / `VKD3D_DEBUG=info` diagnostic additions were removed. Launcher configuration values match the backup (only a final newline differs).
- The virtual pointer's temporary flat-acceleration profile was restored to adaptive before stopping its helper. The game was terminated deliberately from the working main menu after validation, and research-launched Heroic/ydotoold were stopped. T3 Code/Codex and unrelated applications were not stopped.

The verified Cyberpunk path is ready for normal user testing. Remaining limitations are explicitly documented rather than hidden behind estimated lighting corrections: finite-format quantization, incomplete proof of moving-edge/reflection quality, untested game integrations and non-exhaustive GPU-lifetime coverage. No DLSSD-equivalent or lossless-quality claim is made.
