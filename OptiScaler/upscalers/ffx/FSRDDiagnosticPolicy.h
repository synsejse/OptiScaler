#pragma once
#include "FSRDDiagnostics.h"

namespace FSRD
{
struct DiagnosticPlan
{
    bool identity;
    bool runDenoiser;
    bool resetDenoiser;
    bool resetUpscaler;
};

// The caller enables options only for normal composition (including UpscalerBypass).
// Only the divide changes AMD's input. Composition switches reset SR, not RR.
// Returning from a bilinear/debug view resets the stale SR history once.
constexpr DiagnosticPlan PlanDiagnostics(uint32_t options, bool bypassDenoiser, bool baseReset,
                                         bool denoiserHistoryValid, uint32_t previousDenoiserOptions,
                                         bool upscalerHistoryValid, uint32_t previousUpscalerOptions,
                                         bool manualReset)
{
    const bool identity = !bypassDenoiser && (options & IdentityDenoiser) != 0;
    const bool runDenoiser = !bypassDenoiser && !identity;
    return { identity, runDenoiser,
             runDenoiser && (baseReset || !denoiserHistoryValid || manualReset ||
                            ((options ^ previousDenoiserOptions) & SkipAlbedoDivide) != 0),
             baseReset || !upscalerHistoryValid ||
                 ((options ^ previousUpscalerOptions) & (AllDiagnosticOptions ^ BypassUpscaler)) != 0 };
}
} // namespace FSRD
