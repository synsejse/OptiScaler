#pragma once

#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <string>

// Research-only probe for one authenticated Cyberpunk executable. Metadata-only
// unless the separate CyberpunkFogCapture INI opt-in AND an explicit one-shot
// FSRRR-fog-capture.request marker are present. That capture copies the original
// target and repeats the authenticated shader only against a private RGBA target;
// it does not implement a fog correction or change the normal RR input mapping.
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
