# Cyberpunk pre-Fog ray regeneration

Implementation status: continuous-mode CPU regression testing is in progress.
The predecessor `b1fcdb09` completed 18,000 consecutive native denoised frames
with movement and a positive user visual check. This does not yet validate the
new continuous ownership/restart implementation in the game.

## What is fixed

Cyberpunk's late RR color already includes fog. Demodulating that combined color
by surface albedo, denoising it, and multiplying back produces material-shaped
contrast and missing haze. Identity denoising cancels most of that error.

The correction denoises surface lighting before the original fog draw, replaces
only scene RGB, preserves the original alpha, then lets Cyberpunk run its own
fog exactly once. It does not estimate a fog layer or split one lighting signal
into guessed diffuse/specular signals. See the [ordering evidence](FSR_RR_FOG_ORDERING.md).

## Enable

Requires the authenticated Cyberpunk executable, compatible AMD RR provider and
the FSR RR backend. In the game-local `OptiScaler.ini`:

```ini
[FSR-RR]
CyberpunkPreFog=true
```

Save and restart. The equivalent checkbox is in **FSR-RR Advanced Settings**.
No capture/probe flag or test button is required in this mode. The overlay shows
**ACTIVE**, denoised frames, history epoch and retained frame records. There is
no 18,000-frame limit and no periodic denoiser-history reset. The default remains
off for other installations; an unsupported executable is not patched.

## Lifetime and transitions

- One AMD context/history per uninterrupted scene configuration; only its first
  frame uses RESET. Exact within-frame input/camera matching remains required.
- At most 64 CPU frame slots plus two prepared producer target sets. A slot is
  reused only after its consumer fence, actual submission returns, newer native
  producer/consumer command-list Reset generations, and all CPU readers.
- CPU reader leases live in native callback scopes, not GPU-retained plans.
  Submission-owned resource graphs remain acyclic.
- Expected scene, native-reset, view, render-size or settings changes stop new
  work and drain old work before starting a fresh context/history. A submitted
  producer with no consumer may retire only after its own fence and newer list
  Reset; it never commits denoiser history.
- Unknown generations, missing returns, failed signals and unsafe dependencies
  are not discarded to keep rendering. An undrainable session stays stopped.
  A partially recorded unsafe dependency still terminates only the authenticated
  game rather than submitting undefined work. No late-RR fallback is used.
- No automatic buffer dumps or per-frame JSON history in continuous mode.
  Diagnostic 32-frame and 18,000-frame modes remain available separately with
  `CyberpunkPreFog=false` and the existing experiment flags, after a restart.

## Scope and remaining validation

The native EXE/shader/resource contracts are deliberately narrow. Frame duration
still comes from CPU intervals between selected native fog draws; independent
native simulation timing/world-origin epoch proof is not established. Existing
texture formats/quantization and the fused AMD denoiser are unchanged. A positive
visual result is not proof of exact DLSSD equivalence or every scene/game build.

Continuous native validation must include more than 18,000 frames, memory
behavior, scene reloads, and quality changes before this mode is declared ready.
