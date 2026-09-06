# FSR RR fidelity research

The [2026-09-06 Cyberpunk results](FSR_RR_RESEARCH_RESULTS.md) include actual texture measurements, the heuristic-bypass run and a benchmark-reload GPU fault. Do not treat the diagnostic build as stability-qualified.

## Dump buffers from the overlay

Open **FSR-RR Advanced Settings → Debug → Dump buffers**. One click queues one frame of the selected FSR-RR feature. No INI change or restart is needed. The overlay reports queued/GPU-wait/completion/error status and provides **Copy dump folder**. **Cancel dump** cancels only an unrecorded request, never work already referenced by the GPU. Repeated clicks are blocked until the previous dump finishes; completed GUI dumps have no lifetime twelve-frame quota.

Use **Debug View = None** to capture the complete processing chain. Other debug modes are captured as selected, without silently changing rendering; bypassed stages are absent and the manifest records their bypass flags. The inventory covers available NGX inputs, OptiScaler's converted buffers, denoised radiance, composed color and the backend output before OptiScaler's sharpening/output scaling/overlay. It does not enumerate unrelated game render targets or proprietary internal AMD textures.

Files are saved beside the game executable, for example:

`FSRRR-captures/dump-20260906-190203-456Z-1-<pid>-<feature>-<frame>/`

The timestamp is UTC with milliseconds; a process-local sequence prevents same-timestamp collisions. Existing directories are never overwritten. Captures can cause a noticeable stutter and consume hundreds of MiB. Both triggers below are bounded to two pending batches, 64 captured textures and 512 MiB of readback per frame. Exceeding a limit or encountering an unsupported present texture produces an explicitly **partial** dump with details in the manifest, not a silent success. Active render rectangles are captured from origin zero; smaller optional textures retain their own dimensions, and backend output uses its full texture extent.

## Automated file trigger

For scripted research, enable `[FSR-RR] ResearchCapture=true`, then place `FSRRR-capture.request` beside the executable containing a short alphanumeric/underscore/hyphen label. This legacy trigger captures the next two evaluations of the selected RR feature, at most twelve frames per process. The file is consumed; both frames share a timestamped label. With the setting disabled (the default) and no GUI request, capture is idle. A pending GUI request is not stolen by another feature or a file request.

## Buffer format and synchronization

The `.f32` files contain shader-visible little-endian float32 RGBA values, row-major, **not native texture bytes**. FP16 inputs expand exactly; normalized integers are decoded by the GPU. Missing channels use the usual SRV defaults (for example alpha one for an R-only texture); these are not additional game-provided channels. No filtering, clamping or channel omission occurs in the copy shader. Typeless views follow the existing bridge's view-format mapping. Unsupported layouts are recorded as metadata only. Missing optional inputs are recorded, not fabricated.

Game inputs are read as shader resources at the RR evaluation point, as required by the existing converter. Only capture-owned buffers are transitioned to copy source. The backend output is temporarily transitioned from its known UAV/restored state to shader-read, then returned to that state. The actual ExecuteCommandLists hook signals a dedicated fence after submitting the containing command list; readbacks are mapped only after successful completion. Source resources, descriptors, PSO and buffers remain owned until then. Failed/unsubmitted batches retain bounded resources until process exit instead of guessing GPU completion. There is no synchronous GPU wait. GPU copies and disk writes are intentionally expensive: capture runs are not performance benchmarks.

The manifest contains raw-input inventory, resource/view formats, active dimensions, available resource debug names, conversion matrices/flags, jitter and motion scales, debug/bypass/reset state and capture completeness. `manifest.json` is published only after successful texture writes and a checked temporary-manifest write. Directories without it are incomplete captures. All four motion channels are preserved for investigation. File names identify stages; absence is recorded rather than inventing input data.

## Historical comparisons

The original research compared the following configurations; these heuristic controls were subsequently removed. See [polish results](FSR_RR_POLISH_RESULTS.md) for current behavior and the unresolved user visual regression.

1. Original fused path: `FloorIsolation=1`, `CorrelationBias=1`.
2. Heuristic bypass reference: `FloorIsolation=0`, `CorrelationBias=0`. This still includes format conversion, albedo sanitization/quantization and camera-only depth delta; it must not be called fully lossless.
3. Quantify each input-to-converted error before deciding whether wider AMD-compatible formats or changed sanitization are justified.

RR 1.1's bundled SDK 2.2 contract is the reference for this branch. Current online RR 1.2 documentation describes a changed API and must not be silently substituted. No assumption is made that unobserved extra motion channels are valid object-depth deltas, or that a two-frame numerical capture establishes temporal image quality.
