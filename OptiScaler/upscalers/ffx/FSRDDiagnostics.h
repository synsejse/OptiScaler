#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace FSRD
{
// Zero is the unmodified production pipeline. One atomic snapshot keeps presets
// and independently combined stage switches consistent for an entire frame.
enum DiagnosticOption : uint32_t
{
    IdentityDenoiser = 1 << 0,
    SkipAlbedoDivide = 1 << 1,
    SkipAlbedoMultiply = 1 << 2,
    SkipResidual = 1 << 3,
    BypassUpscaler = 1 << 4,
    AllDiagnosticOptions = (1 << 5) - 1,
};

// Owned by one feature. Exposed through IFeature's optional capability, never by
// downcasting its virtual base (RTTI is disabled in the release build).
struct Diagnostics
{
    static constexpr uint64_t NoDiagnosticFrame = (std::numeric_limits<uint64_t>::max)();

    uint32_t Options() const { return options.load(); }
    void SetOptions(uint32_t value) { options.store(value & AllDiagnosticOptions); }
    bool DenoiserResetPending() const { return resetDenoiserHistory.load(); }
    void RequestDenoiserReset() { resetDenoiserHistory.store(true); }
    void CancelDenoiserReset() { resetDenoiserHistory.store(false); }
    uint64_t LastDiagnosticResetFrame() const { return lastResetFrame.load(); }

    // Render-thread access is atomic too; no GPU work is done by the menu.
    std::atomic<uint32_t> options { 0 };
    std::atomic<bool> resetDenoiserHistory { false };
    std::atomic<uint64_t> lastResetFrame { NoDiagnosticFrame };
};
} // namespace FSRD
