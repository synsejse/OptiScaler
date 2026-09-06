#include "../OptiScaler/upscalers/ffx/FSRDDiagnosticPolicy.h"
#include <cassert>
#include <iostream>

using FSRD::PlanDiagnostics;

int main()
{
    // Ordinary rendering is unchanged after initialization.
    constexpr auto normal = PlanDiagnostics(true, false, false, false, true, false, false);
    static_assert(!normal.identity && normal.runDenoiser && !normal.resetDenoiser && !normal.resetUpscaler);
    constexpr auto initial = PlanDiagnostics(true, false, false, true, false, false, false);
    static_assert(initial.runDenoiser && initial.resetDenoiser && initial.resetUpscaler);

    // The manual reset changes RR only, preserving SR and camera history.
    constexpr auto manual = PlanDiagnostics(true, false, false, false, true, false, true);
    static_assert(manual.runDenoiser && manual.resetDenoiser && !manual.resetUpscaler);
    constexpr auto gameReset = PlanDiagnostics(true, false, false, true, true, false, true);
    static_assert(gameReset.resetDenoiser && gameReset.resetUpscaler);

    // Identity retains conversion/composition/SR, but never dispatches the AMD denoiser.
    constexpr auto enter = PlanDiagnostics(true, true, false, false, true, false, false);
    static_assert(enter.identity && !enter.runDenoiser && !enter.resetDenoiser && enter.resetUpscaler);
    constexpr auto steady = PlanDiagnostics(true, true, false, false, false, true, false);
    static_assert(steady.identity && !steady.runDenoiser && !steady.resetDenoiser && !steady.resetUpscaler);
    constexpr auto leave = PlanDiagnostics(true, false, false, false, false, true, false);
    static_assert(!leave.identity && leave.runDenoiser && leave.resetDenoiser && leave.resetUpscaler);

    // RR output debug views still filter. Identity never overrides a selected debug view.
    constexpr auto outputDebug = PlanDiagnostics(false, true, false, false, true, false, false);
    static_assert(!outputDebug.identity && outputDebug.runDenoiser && !outputDebug.resetDenoiser);
    constexpr auto bypassDebug = PlanDiagnostics(false, true, true, false, true, false, false);
    static_assert(!bypassDebug.identity && !bypassDebug.runDenoiser && !bypassDebug.resetDenoiser);
    constexpr auto historyLost = PlanDiagnostics(true, false, false, false, false, false, false);
    static_assert(historyLost.resetDenoiser && !historyLost.resetUpscaler);
    // An intervening output-only debug view cannot commit an SR source change.
    constexpr auto debugAfterIdentity = PlanDiagnostics(false, false, false, false, false, true, false);
    static_assert(!debugAfterIdentity.identity && debugAfterIdentity.runDenoiser && debugAfterIdentity.resetDenoiser);
    constexpr auto resumeAfterDebug = PlanDiagnostics(true, false, false, false, true, true, false);
    static_assert(!resumeAfterDebug.resetDenoiser && resumeAfterDebug.resetUpscaler);

    // Model the successful-frame commits over a long identity sequence: only one SR reset
    // at each source transition, RR history remains invalid until AMD executes again.
    bool history = true;
    bool previousIdentity = false;
    unsigned srResets = 0;
    unsigned rrResets = 0;
    for (unsigned frame = 0; frame < 100; ++frame)
    {
        const bool requested = frame >= 10 && frame < 90;
        const auto plan = PlanDiagnostics(true, requested, false, false, history, previousIdentity, false);
        srResets += plan.resetUpscaler;
        rrResets += plan.resetDenoiser;
        history = plan.runDenoiser;
        previousIdentity = plan.identity;
    }
    assert(srResets == 2 && rrResets == 1 && history && !previousIdentity);
    // Failed evaluations invalidate camera history: both histories reset on retry.
    const auto retry = PlanDiagnostics(true, false, false, true, false, previousIdentity, false);
    assert(retry.resetDenoiser && retry.resetUpscaler);
    std::cout << "FSRD diagnostic policy tests passed\n";
}
