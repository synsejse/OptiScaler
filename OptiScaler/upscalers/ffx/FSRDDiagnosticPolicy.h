#pragma once

namespace FSRD
{
struct DiagnosticPlan
{
    bool identity;
    bool runDenoiser;
    bool resetDenoiser;
    bool resetUpscaler;
};

// Identity is deliberately limited to the normal full processing chain. A manual
// reset affects only RR; switching color sources resets SR once to avoid mixing
// old filtered output with the identity result. Existing game resets still apply.
constexpr DiagnosticPlan PlanDiagnostics(bool normalView, bool identityRequested, bool bypassDenoiser,
                                         bool baseReset, bool denoiserHistoryValid, bool previousIdentity,
                                         bool manualReset)
{
    const bool identity = normalView && identityRequested;
    const bool runDenoiser = !bypassDenoiser && !identity;
    return { identity, runDenoiser,
             runDenoiser && (baseReset || !denoiserHistoryValid || manualReset),
             baseReset || identity != previousIdentity };
}
} // namespace FSRD
