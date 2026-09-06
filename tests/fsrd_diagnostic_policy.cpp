#include "../OptiScaler/upscalers/ffx/FSRDDiagnosticPolicy.h"
#include <cassert>
#include <iostream>

using namespace FSRD;

int main()
{
    Diagnostics controls;
    assert(controls.Options() == 0 && !controls.DenoiserResetPending());
    assert(!controls.NativeDebugAvailable() && !controls.ShowNativeDebug() && controls.NativeDebugSelection() == 0);
    controls.SetShowNativeDebug(true);
    controls.SetNativeDebugSelection(12);
    assert(controls.ShowNativeDebug() && controls.NativeDebugSelection() == 12 && controls.Options() == 0);
    controls.SetNativeDebugSelection(13);
    assert(controls.NativeDebugSelection() == 0);
    assert(controls.LastDiagnosticResetFrame() == Diagnostics::NoDiagnosticFrame);
    controls.SetOptions(IdentityDenoiser | BypassUpscaler);
    controls.RequestDenoiserReset();
    assert(controls.Options() == (IdentityDenoiser | BypassUpscaler) && controls.DenoiserResetPending());
    controls.CancelDenoiserReset();
    assert(!controls.DenoiserResetPending());
    controls.SetOptions(~uint32_t(0));
    assert(controls.Options() == AllDiagnosticOptions);
    controls.lastResetFrame.store(123);
    assert(controls.LastDiagnosticResetFrame() == 123);
    Diagnostics recreated;
    assert(recreated.Options() == 0 && !recreated.DenoiserResetPending());

    constexpr auto normal = PlanDiagnostics(0, false, false, true, 0, true, 0, false);
    static_assert(!normal.identity && normal.runDenoiser && !normal.resetDenoiser && !normal.resetUpscaler);
    constexpr auto initial = PlanDiagnostics(0, false, true, false, 0, false, 0, false);
    static_assert(initial.resetDenoiser && initial.resetUpscaler);
    constexpr auto manual = PlanDiagnostics(0, false, false, true, 0, true, 0, true);
    static_assert(manual.resetDenoiser && !manual.resetUpscaler);

    // Every independent combination, both transitions and steady state. Returning
    // from identity invalidates AMD history; SR never commits a bilinear frame.
    for (uint32_t before = 0; before <= AllDiagnosticOptions; ++before)
    for (uint32_t now = 0; now <= AllDiagnosticOptions; ++now)
    {
        const bool rrHistory = !(before & IdentityDenoiser);
        const bool srHistory = !(before & BypassUpscaler);
        const auto p = PlanDiagnostics(now, false, false, rrHistory, before, srHistory, before, false);
        assert(p.identity == bool(now & IdentityDenoiser));
        assert(p.runDenoiser == !p.identity);
        assert(p.resetDenoiser == (p.runDenoiser &&
            (!rrHistory || bool((now ^ before) & SkipAlbedoDivide))));
        assert(p.resetUpscaler == (!srHistory ||
            bool((now ^ before) & (AllDiagnosticOptions ^ BypassUpscaler))));
        const auto manual = PlanDiagnostics(now, false, false, rrHistory, before, srHistory, before, true);
        assert(manual.resetDenoiser == manual.runDenoiser);
        assert(manual.resetUpscaler == p.resetUpscaler);
    }

    constexpr auto identityBilinear = PlanDiagnostics(IdentityDenoiser | BypassUpscaler,
        false, false, true, 0, true, 0, false);
    static_assert(identityBilinear.identity && !identityBilinear.runDenoiser);
    constexpr auto compositionOnly = PlanDiagnostics(SkipAlbedoMultiply, false, false, true, 0, true, 0, false);
    static_assert(!compositionOnly.resetDenoiser && compositionOnly.resetUpscaler);
    constexpr auto rawReference = PlanDiagnostics(0, true, false, true, 0, true, 0, false);
    static_assert(!rawReference.identity && !rawReference.runDenoiser && !rawReference.resetDenoiser);

    // Each transition resets once, not indefinitely.
    bool rrHistory = true, srHistory = true;
    uint32_t previousRR = 0, previousSR = 0;
    unsigned srResets = 0, rrResets = 0;
    for (unsigned frame = 0; frame < 100; ++frame)
    {
        const uint32_t options = frame >= 10 && frame < 90 ? uint32_t(IdentityDenoiser) : 0;
        const auto p = PlanDiagnostics(options, false, false, rrHistory, previousRR, srHistory, previousSR, false);
        srResets += p.resetUpscaler;
        rrResets += p.resetDenoiser;
        rrHistory = p.runDenoiser;
        srHistory = true;
        previousRR = previousSR = options;
    }
    assert(srResets == 2 && rrResets == 1 && rrHistory);
    const auto retry = PlanDiagnostics(0, false, true, false, previousRR, false, previousSR, false);
    assert(retry.resetDenoiser && retry.resetUpscaler);
    std::cout << "FSRD diagnostic policy tests passed (1024 switch transitions)\n";
}
