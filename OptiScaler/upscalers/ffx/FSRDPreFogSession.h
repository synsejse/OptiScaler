#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace FSRD::PreFogSession
{
// One process-owned routing choice. Neither a live INI edit, feature recreation,
// early CPU recording order nor a latest diagnostic receipt may switch routes.
// Before the first FSRD feature freezes configuration, scene insertion is refused.
class RouteLatch
{
    enum class Route : uint8_t { Uninitialized, LateRr, LateSrOnly };
    std::atomic<Route> _route { Route::Uninitialized };

  public:
    bool Freeze(bool requested) noexcept
    {
        auto expected = Route::Uninitialized;
        _route.compare_exchange_strong(expected, requested ? Route::LateSrOnly : Route::LateRr,
                                       std::memory_order_acq_rel, std::memory_order_acquire);
        return LateSrOnly();
    }
    bool LateSrOnly() const noexcept { return _route.load(std::memory_order_acquire) == Route::LateSrOnly; }
};

inline RouteLatch processRoute;
inline bool Freeze(bool requested) noexcept { return processRoute.Freeze(requested); }
inline bool LateSrOnly() noexcept { return processRoute.LateSrOnly(); }
inline std::string_view StatusText() noexcept
{
    return LateSrOnly()
        ? "Pre-Fog experiment: late SR only; early scene denoising is one-shot and NOT guaranteed for this frame. Missing early work remains noisy; no late RR fallback."
        : "Ordinary late ray regeneration; pre-Fog scene experiment disabled.";
}

inline bool Finite(float value) noexcept
{
    // /fp:fast must not turn nonfinite input admission into an unconditional true.
    return (std::bit_cast<uint32_t>(value) & 0x7f800000u) != 0x7f800000u;
}

// Current-call scalar extraction only; no early camera, last-frame matrix, RR
// resource gathering, inferred near/far planes or exposure transformation.
inline bool VerticalFov(const std::array<float, 16>& projection, float& result) noexcept
{
    result = 0;
    for (const float value : projection)
        if (!Finite(value) || std::abs(value) >= 1e30f) return false;
    // The current DLSSD conventional perspective scale is positive. A singular,
    // orthographic or nonseparable projection is unsupported, never repaired.
    if (projection[0] <= 0 || projection[5] <= 0 || projection[15] != 0 ||
        std::abs(projection[11]) != 1 || projection[14] == 0 ||
        projection[1] != 0 || projection[2] != 0 || projection[3] != 0 ||
        projection[4] != 0 || projection[6] != 0 || projection[7] != 0 ||
        projection[12] != 0 || projection[13] != 0) return false;
    result = float(2.0 * std::atan(1.0 / double(projection[5])));
    return Finite(result) && result > 0 && result < 3.14159265358979323846;
}

// Per-feature SR history, independent of all denoiser/camera histories. Begin is
// called on the ACTUAL SR descriptor immediately before its dispatch, so render
// and output-size overrides are included. Only successful base SR commits it.
class LateSrHistory
{
    std::array<uint32_t, 4> _extent {};
    bool _valid = false, _pending = false;

  public:
    bool Begin(uint32_t width, uint32_t height, uint32_t outputWidth, uint32_t outputHeight,
               bool gameReset) noexcept
    {
        const std::array<uint32_t, 4> extent { width, height, outputWidth, outputHeight };
        const bool reset = gameReset || !_valid || extent != _extent;
        _extent = extent;
        _valid = false;
        _pending = width && height && outputWidth && outputHeight;
        return reset;
    }
    void Complete(bool succeeded) noexcept { _valid = succeeded && _pending; _pending = false; }
    void Invalidate() noexcept { _valid = _pending = false; }
};
} // namespace FSRD::PreFogSession
