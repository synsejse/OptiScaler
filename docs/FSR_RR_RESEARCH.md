# FSR RR fidelity research

The [2026-09-06 Cyberpunk results](FSR_RR_RESEARCH_RESULTS.md) include actual texture measurements, the heuristic-bypass run and a benchmark-reload GPU fault. Do not treat the diagnostic build as stability-qualified.

This branch adds opt-in diagnostic capture, not a new quality preset. Normal rendering is unchanged when `[FSR-RR] ResearchCapture=false` (the default).

With the setting enabled, place `FSRRR-capture.request` beside the executable containing a short alphanumeric/underscore/hyphen label. The next two evaluations of the selected RR feature capture the inputs and converted resources. Results appear under `FSRRR-captures/<label>-<pid>-<feature>-<frame>/`. The trigger is consumed. Limits: twelve frames per process, two pending batches, 48 textures and 512 MiB readback per frame. Full active render rectangles are captured from origin zero; smaller optional textures retain their own dimensions.

The `.f32` files contain shader-visible little-endian float32 RGBA values, row-major, **not native texture bytes**. FP16 inputs expand exactly; normalized integers are decoded by the GPU. Missing channels use the usual SRV defaults (for example alpha one for an R-only texture); these are not additional game-provided channels. No filtering, clamping or channel omission occurs in the copy shader. Typeless views follow the existing bridge's view-format mapping. Unsupported layouts are recorded as metadata only. Missing optional inputs are recorded, not fabricated.

Game inputs are read as shader resources at the RR evaluation point, as required by the existing converter. Only capture-owned buffers are transitioned to copy source. The actual ExecuteCommandLists hook signals a dedicated fence after submitting the containing command list; readbacks are mapped only after successful completion. Source resources, descriptors, PSO and buffers remain owned until then. Failed/unsubmitted batches retain bounded resources until process exit instead of guessing GPU completion. There is no synchronous GPU wait. GPU copies and disk writes are intentionally expensive: capture runs are not performance benchmarks.

The manifest contains raw-input inventory, resource/view formats, active dimensions, conversion matrices/flags, jitter and motion scales, floor/correlation settings and reset state. A manifest is written after the texture files; directories without one are incomplete captures. All four motion channels are preserved for investigation, without assigning unverified semantics to Z/W.

Research comparisons:

1. Current fused path: `FloorIsolation=1`, `CorrelationBias=1`.
2. Heuristic bypass reference: `FloorIsolation=0`, `CorrelationBias=0`. This still includes format conversion, albedo sanitization/quantization and camera-only depth delta; it must not be called fully lossless.
3. Quantify each input-to-converted error before deciding whether wider AMD-compatible formats or changed sanitization are justified.

RR 1.1's bundled SDK 2.2 contract is the reference for this branch. Current online RR 1.2 documentation describes a changed API and must not be silently substituted. No assumption is made that unobserved extra motion channels are valid object-depth deltas, or that a two-frame numerical capture establishes temporal image quality.
