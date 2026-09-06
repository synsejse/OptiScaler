#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace FSRD
{
// Owned by one feature. Exposed through IFeature's optional capability, never by
// downcasting its virtual base (RTTI is disabled in the release build).
struct Diagnostics
{
    static constexpr uint64_t NoDiagnosticFrame = (std::numeric_limits<uint64_t>::max)();

    bool IdentityDenoiserRequested() const { return identityDenoiser.load(); }
    void SetIdentityDenoiser(bool enabled) { identityDenoiser.store(enabled); }
    bool DenoiserResetPending() const { return resetDenoiserHistory.load(); }
    void RequestDenoiserReset() { resetDenoiserHistory.store(true); }
    void CancelDenoiserReset() { resetDenoiserHistory.store(false); }
    uint64_t LastDiagnosticResetFrame() const { return lastResetFrame.load(); }

    // Render-thread access is atomic too; no GPU work is done by the menu.
    std::atomic<bool> identityDenoiser { false };
    std::atomic<bool> resetDenoiserHistory { false };
    std::atomic<uint64_t> lastResetFrame { NoDiagnosticFrame };
};
} // namespace FSRD
