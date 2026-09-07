#pragma once

#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <string>

namespace FSRD { struct DenoiserSettings; }

// Probe and pre-Fog insertion for one authenticated Cyberpunk executable. Metadata-only
// unless the separate CyberpunkFogCapture INI opt-in AND an explicit one-shot
// capture/control marker are present. FSRRR-fog-capture.request copies the original
// target and repeats the authenticated shader only against a private RGBA target;
// The separate restart-fixed CyberpunkPreFog option enables continuous insertion
// before original fog; it does not rewrite the normal NGX RR input mapping.
// Initialize at device setup, never under DllMain's loader lock. Once installed,
// the hooks remain until process exit; changing the opt-in requires a restart.
// Creating FSRRR-fog-probe.request beside the game exe re-arms the first-32-draw
// log budget (at most twice/process); the marker is consumed, no game files changed.
namespace FSRDCyberpunkFogProbe
{
void Initialize(bool enabled);
void HookDevice(ID3D12Device* device);
void HookCommandList(ID3D12GraphicsCommandList* commandList);
// Called on the initialized late RR feature's own thread. Only allocates a
// one-shot packet for an explicit FSRRR-prefog-reset.request JSON marker; its
// supplied extent is allocation metadata, never a future frame association.
void ArmPrivateReset(ID3D12Device* device, UINT width, UINT height) noexcept;
// The same explicit RESET marker may select "scene_reset_once" only in a
// restart-fixed CyberpunkPreFogExperiment session. One composed private RESET
// result then replaces original pre-Fog RGB, never alpha. Original Fog runs once;
// late evaluations stay SR-only. This is NOT continuous temporal denoising.
// Separate one-shot FSRRR-prefog-rgb-identity.request JSON marker with exactly
// {"mode":"rgb_identity_only"}. Requires an unused authenticated capture session.
// Copies pre-Fog RGB back through the approved RGB-only raster helper, snapshots
// the result, then resumes the original Fog draw once. Original alpha is unwritten;
// actual identity is tested from the snapshots, NOT assumed. No guide/RESET packet,
// denoised scene substitution, or change to the ordinary late RR/SR route.
void ArmRgbIdentity(ID3D12Device* device, UINT width, UINT height) noexcept;
// Explicit disabled-by-default FSRRR-prefog-temporal.request experiment: one
// persistent Session: 32-frame capture or 18000-frame visual pass, first RESET
// then owned previous camera. Requires restart-fixed late SR. Timing is the
// selected original Fog draw CPU interval, not an authenticated engine delta.
// Poll also retires fence-complete owners; it never waits or enables a fallback.
// In CyberpunkPreFog mode it starts automatically with 64 reusable CPU slots,
// no frame limit/readbacks, and new histories only after a drained scene change.
void PollTemporalWindow(ID3D12Device* device, UINT width, UINT height, uint64_t provider = 0,
                        const FSRD::DenoiserSettings* settings = nullptr) noexcept;
enum class TemporalTestPhase : uint8_t { Unavailable, Ready, Requested, Refused, Warmup, Active, Stalled, Stopped, Complete, Restarting };
struct TemporalTestStatus
{
    TemporalTestPhase phase = TemporalTestPhase::Unavailable;
    uint32_t frames = 0, limit = 0;
    uint64_t epoch = 0;
    uint32_t retainedFrames = 0;
};
TemporalTestStatus GetTemporalTestStatus() noexcept;
bool RequestVisualTest() noexcept; // GUI requests only; native/provider work stays on the feature caller.
// Mandatory dependency veto BEFORE both native Execute branches and their
// FSRDSubmission::Preparing calls. A refusal terminates this authenticated game
// diagnostic, never drops a native list or pretends that submission succeeded.
uint64_t AdmitPrivateResetSubmission(ID3D12CommandQueue* queue, UINT count,
                                     ID3D12CommandList* const* lists) noexcept;
void ReturnedPrivateResetSubmission(uint64_t token) noexcept;
// Call before RR input conversion with the actual NGX resource pointers. No-op
// except for eight endpoint observations after an explicitly requested fog capture.
// At most two matching endpoints receive immutable immediate-capture candidates.
// Reports resource identity and local recording order, NOT GPU/frame association
// or unchanged contents. It records no GPU commands and changes no NGX inputs.
struct NgxCaptureCandidate
{
    std::string metadata; // JSON: unvalidated candidate, never a positive fog_pairing claim.
    unsigned index = 0;
    std::shared_ptr<void> ownership;
};
using CaptureCandidate = std::shared_ptr<const NgxCaptureCandidate>;
CaptureCandidate ObserveNgxInput(ID3D12GraphicsCommandList* commandList, ID3D12Resource* color,
                     ID3D12Resource* colorBeforeParticles, uint64_t featureId, uint64_t frameIndex,
                     UINT renderWidth, UINT renderHeight) noexcept;
// Report this exact Evaluate's forced Begin result, including false on an early
// return. A candidate is consumed when issued: failed work is never re-queued.
void CandidateCaptureResult(const CaptureCandidate& candidate, bool started) noexcept;

struct SubmissionObservation;
using SubmissionObservationToken = std::shared_ptr<SubmissionObservation>;
SubmissionObservationToken PreparingSubmission(ID3D12CommandQueue* queue, UINT count,
                                                ID3D12CommandList* const* lists) noexcept;
void SubmittedSubmission(const SubmissionObservationToken& observation) noexcept;
} // namespace FSRDCyberpunkFogProbe
