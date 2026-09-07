#pragma once

#include "FSRDCyberpunkResetCamera.h"

namespace FSRD::CyberpunkTemporalCamera
{
using Snapshot = CyberpunkResetCamera::Snapshot;
using Parameters = CyberpunkResetCamera::Parameters;

// Required current observation, never inferred from a missing field or a prior
// frame. Requested means the authenticated native view+0xef0 source was zero.
enum class NativeReset : uint8_t { Unavailable, NotRequested, Requested };

// OWN immutable copy of the preceding accepted frame, not a borrowed engine
// pointer or a parameter block obtained from a late/other-view evaluation.
struct PreviousFrame
{
    Snapshot source {};
    std::array<float, 2> motionScale {};
    float deltaMilliseconds = 0;
    uint32_t frameIndex = 0;
    uint64_t sessionEpoch = 0;
};

// Explicit caller attestations. The builder cannot discover native view/origin
// lifetime, successful recording, or a software session from camera similarity.
// These do NOT prove GPU ordering: the host must enforce the preceding denoiser
// consumer -> current consumer dependency before submission and retain owners.
struct Continuity
{
    uint64_t sessionEpoch = 0;
    bool sameViewAndCoordinateOrigin = false;
    bool previousDispatchAccepted = false;
};

#if defined(_MSC_VER) || defined(__clang__)
#pragma float_control(precise, on, push)
#endif
#if defined(_MSC_VER)
#define FSRD_TEMPORAL_CAMERA_PRECISE __declspec(noinline)
#elif defined(__GNUC__) && !defined(__clang__)
#define FSRD_TEMPORAL_CAMERA_PRECISE __attribute__((noinline, optimize("no-fast-math", "fp-contract=off")))
#elif defined(__clang__)
#define FSRD_TEMPORAL_CAMERA_PRECISE __attribute__((noinline))
#else
#define FSRD_TEMPORAL_CAMERA_PRECISE
#endif

// A missing predecessor is an explicit first/invalidated-history RESET choice.
// Native reset also takes precedence over any stored predecessor. Otherwise a
// supplied predecessor MUST pass every continuity check; refusal never silently
// invents history or resets an allegedly continuous dispatch. Counter wrap must
// be explicitly reset by the host rather than inferred from two uint32 values.
// deltaMilliseconds is a required current caller measurement with documented
// provenance; there is no engine clock, fixed-duration default, or late borrowing.
//
// This helper only constructs constants. It neither removes Work's independent
// one-shot RESET policy nor makes a persistent context/thread/queue safe. The
// host separately authenticates the native RGBA16F depth-motion producer; bit5
// enables that existing exact encoding/decoder, not arbitrary four-channel MV.
// Unknown/clamped per-pixel motion still follows the converter's documented
// fallback policy; this is not a claim of lossless recovery of those samples.
// Every refusal leaves output unchanged; all source copies are read-only.
FSRD_TEMPORAL_CAMERA_PRECISE inline bool Build(
    const Snapshot& current, const std::array<float, 2>& currentMotionScale,
    float deltaMilliseconds, uint32_t frameIndex, NativeReset nativeReset,
    const PreviousFrame* previous, const Continuity& continuity, Parameters& output) noexcept
{
    if (!continuity.sessionEpoch ||
        (nativeReset != NativeReset::NotRequested && nativeReset != NativeReset::Requested)) return false;
    Parameters result;
    if (!CyberpunkResetCamera::Build(current, currentMotionScale, deltaMilliseconds, frameIndex, result)) return false;
    if (!previous || nativeReset == NativeReset::Requested)
    {
        output = result;
        return true;
    }

    if (!continuity.sameViewAndCoordinateOrigin || !continuity.previousDispatchAccepted ||
        previous->sessionEpoch != continuity.sessionEpoch ||
        previous->frameIndex == std::numeric_limits<uint32_t>::max() || frameIndex != previous->frameIndex + 1u ||
        previous->source.width != current.width || previous->source.height != current.height ||
        previous->source.projectionFlags != current.projectionFlags) return false;
    Parameters prior;
    if (!CyberpunkResetCamera::Build(previous->source, previous->motionScale, previous->deltaMilliseconds,
                                     previous->frameIndex, prior)) return false;

    result.previousView = previous->source.nativeView;
    // The reset builder's historical coefficients are exactly that source's
    // current Pd[10], Pd[14], Pd[11]. Preserve those bits, never refit near/far.
    result.previousDepthProjection = prior.previousDepthProjection;
    for (size_t i = 0; i < result.cameraPositionDelta.size(); ++i)
    {
        const float before = std::bit_cast<float>(previous->source.inverseNativeView[12 + i]);
        const float now = std::bit_cast<float>(current.inverseNativeView[12 + i]);
        // Classify before the FP32 operation: an inherited FTZ mode could hide
        // a real subnormal difference by returning zero. Widening keeps such
        // cancellation results normal; it is only a range check, not the output
        // or a claim that every widely separated float difference is exact.
        const double widened = double(before) - double(now);
        const double magnitude = std::abs(widened);
        if (magnitude > double(std::numeric_limits<float>::max()) ||
            (widened != 0.0 && magnitude < double(std::numeric_limits<float>::min()))) return false;
        volatile float difference = before - now;
        if (!CyberpunkResetCamera::Detail::NormalOrZero(float(difference))) return false;
        result.cameraPositionDelta[i] = difference;
    }
    constexpr uint32_t CyberpunkDepthMotion = 1u << 5;
    constexpr uint32_t ResetMotionHistory = 1u << 6;
    constexpr uint32_t DispatchReset = 1u << 0;
    result.conversionFlags = (result.conversionFlags & ~ResetMotionHistory) | CyberpunkDepthMotion;
    result.dispatchFlags &= ~DispatchReset;
    // Current normalized XY is untouched; converted Z is a linear-depth delta
    // in scene units, so its AMD scale remains the reset builder's exact one.
    output = result;
    return true;
}

#undef FSRD_TEMPORAL_CAMERA_PRECISE
#if defined(_MSC_VER) || defined(__clang__)
#pragma float_control(pop)
#endif
} // namespace FSRD::CyberpunkTemporalCamera
