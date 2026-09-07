#pragma once

#include "FSRDInputMath.h"
#include <array>
#include <bit>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <limits>
#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace FSRD::CyberpunkResetCamera
{
using MatrixWords = std::array<uint32_t, 16>;

// Plain current-view source words only. Native matrices have consecutive rows,
// not the transposed CPU representation used by the late NGX feature.
// Host authenticates image/source layout, current scope/view/frame, snapshot
// stability and native resource association. This pure builder establishes none
// of that provenance and reads no pointers, globals, previous frames or defaults.
struct Snapshot
{
    MatrixWords nativeView {};            // view+0xc0
    MatrixWords inverseNativeView {};     // view+0x180
    MatrixWords nativeProjection {};      // view+0x200, current jitter, forward hardware depth
    MatrixWords depthProjection {};       // view+0x360, current jitter, selected depth convention
    std::array<uint32_t, 2> lensOffset {}; // float bits view+0xa0/a4, NOT temporal jitter
    std::array<uint32_t, 2> jitterPixels {}; // native bits view+0x3e0/3e4; Y has NOT been SL-negated
    uint32_t width = 0, height = 0;        // view+0x34/38
    uint32_t jitterWidth = 0, jitterHeight = 0; // view+0x3e8/3ec
    uint8_t projectionFlags = 0;          // view+0x3f4; only authored reverse-Z mask0x04 admitted
};

struct Parameters
{
    // Copy these words directly into the converter's XMFLOAT4X4 storage. HLSL's
    // column-major interpretation supplies the required transpose; do NOT add one.
    MatrixWords inverseView {}, inverseProjection {}, previousView {};
    std::array<float, 4> renderSize {}, previousDepthProjection {};
    std::array<float, 3> cameraRight {}, cameraUp {}, cameraForward {}, cameraPositionDelta {};
    std::array<float, 3> motionScale {};
    std::array<float, 2> jitterNdc {};
    float nearPlane = 0, farPlane = 0, aspectRatio = 0, verticalFovRadians = 0, deltaMilliseconds = 0;
    uint32_t frameIndex = 0;
    uint32_t conversionFlags = 0, dispatchFlags = 0;
    bool infinitePlanePolicy = false;
};

// Mirrored wire flags only; no dependency on D3D12/FFX/NGX types or state.
// Current Cyberpunk: linear albedos, normal.W roughness, hardware depth, +Z view.
inline constexpr uint32_t ConversionNonGammaPackedReset = (1u << 0) | (1u << 2) | (1u << 6);
inline constexpr uint32_t DispatchResetNonGamma = (1u << 0) | (1u << 1);

// The game's source recipe uses separate float32 arithmetic. Do not let the
// project's /fp:fast replace its jitter division, collapse finite checks or fuse
// multiply/add. Never change the executing render thread's FP control state.
#if defined(_MSC_VER) || defined(__clang__)
#pragma float_control(precise, on, push)
#endif
#if defined(_MSC_VER)
#define FSRD_RESET_CAMERA_PRECISE __declspec(noinline)
#elif defined(__GNUC__) && !defined(__clang__)
#define FSRD_RESET_CAMERA_PRECISE __attribute__((noinline, optimize("no-fast-math", "fp-contract=off")))
#elif defined(__clang__)
#define FSRD_RESET_CAMERA_PRECISE __attribute__((noinline))
#else
#define FSRD_RESET_CAMERA_PRECISE
#endif

namespace Detail
{
inline float Float(uint32_t bits) noexcept { return std::bit_cast<float>(bits); }
inline bool NormalOrZero(uint32_t bits) noexcept
{
    const uint32_t exponent = bits & 0x7f800000u;
    return exponent != 0x7f800000u && (exponent != 0 || (bits & 0x007fffffu) == 0);
}
inline bool NormalOrZero(float value) noexcept { return NormalOrZero(std::bit_cast<uint32_t>(value)); }
inline bool Equal(float left, float right) noexcept
{
    const auto a = std::bit_cast<uint32_t>(left), b = std::bit_cast<uint32_t>(right);
    return a == b || (((a | b) & 0x7fffffffu) == 0); // Signed-zero arithmetic is equivalent here.
}
inline bool NearestRounding() noexcept
{
    if (std::fegetround() != FE_TONEAREST) return false;
#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__)
    if ((_mm_getcsr() & 0x6000u) != 0) return false;
#endif
    return true;
}
using Matrix = std::array<float, 16>;
inline bool Decode(const MatrixWords& words, Matrix& matrix) noexcept
{
    for (size_t i = 0; i < words.size(); ++i)
    {
        if (!NormalOrZero(words[i])) return false;
        matrix[i] = Float(words[i]);
    }
    return true;
}
FSRD_RESET_CAMERA_PRECISE inline bool Perspective(const Matrix& p) noexcept
{
    // Exact authored finite/infinite positive-Z perspective shape. Reject
    // orthographic, oblique or generic matrices instead of inferring a camera.
    constexpr size_t zero[] = { 1, 2, 3, 4, 6, 7, 12, 13, 15 };
    for (size_t i : zero) if (p[i] != 0.0f) return false;
    return p[0] > 0.0f && p[5] > 0.0f && p[10] >= 1.0f && p[11] == 1.0f && p[14] < 0.0f;
}
FSRD_RESET_CAMERA_PRECISE inline bool RigidInverse(const Matrix& view, const Matrix& inverse) noexcept
{
    for (const auto* matrix : { &view, &inverse })
        if ((*matrix)[3] != 0 || (*matrix)[7] != 0 || (*matrix)[11] != 0 || (*matrix)[15] != 1)
            return false;
    // Authored rigid-inverse helper transposes the rotation without changing its
    // values. Do not require determinant +1: the engine's world/view axes can reflect.
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
            if (!Equal(inverse[row * 4 + column], view[column * 4 + row])) return false;
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
        {
            double dot = 0;
            for (size_t k = 0; k < 3; ++k)
                dot += double(inverse[row * 4 + k]) * inverse[column * 4 + k];
            if (std::abs(dot - (row == column ? 1.0 : 0.0)) > 2e-5) return false;
        }
    // Cancellation-aware consistency bound, not an exact inverse oracle or a
    // correction. The native translated rigid inverse has real FP32 rounding.
    for (size_t column = 0; column < 3; ++column)
    {
        double sum = inverse[12 + column], magnitude = std::abs(sum);
        for (size_t k = 0; k < 3; ++k)
        {
            const double product = double(view[12 + k]) * inverse[k * 4 + column];
            sum += product; magnitude += std::abs(product);
        }
        if (std::abs(sum) > 32.0 * std::numeric_limits<float>::epsilon() * (1.0 + magnitude)) return false;
    }
    return true;
}
FSRD_RESET_CAMERA_PRECISE inline std::array<float, 3> UnitRow(const Matrix& inverse, size_t row) noexcept
{
    const float x = inverse[row * 4], y = inverse[row * 4 + 1], z = inverse[row * 4 + 2];
    const float length = std::sqrt((x * x + y * y) + z * z);
    return { x / length, y / length, z / length };
}
} // namespace Detail

// currentMotionScale is the observed native normalized-space DLSS/MvecScaleX/Y
// pair: do not divide it by render dimensions again. measuredDeltaMilliseconds
// and frameIndex are explicit caller-owned measurements, not values fabricated
// here. A callback interval is a documented timing policy, not an engine-clock claim.
// The independent one-shot context makes current-as-previous / zero camera delta
// the explicit RESET baseline; it does not justify unknown actual native motion XY.
//
// The inverse is an analytic, precisely rounded inverse of the admitted canonical
// no-jitter Pd, not a claim of bit identity with DirectXMath's general inverse.
// Existing GetViewPlanes infinite-threshold policy is deliberately preserved.
// The resulting parameters authorize NO resource borrow, GPU ordering, or scene
// write. A later visual correction must use exact pre-Fog color, not silently
// equate an earlier final-lighting MRT with the successful pre-Fog replay boundary.
// Every refusal leaves the complete output unchanged.
FSRD_RESET_CAMERA_PRECISE inline bool Build(const Snapshot& source, const std::array<float, 2>& currentMotionScale,
                                            float measuredDeltaMilliseconds, uint32_t frameIndex,
                                            Parameters& output) noexcept
{
    using namespace Detail;
    if (!NearestRounding() || !source.width || !source.height || source.width > 8192 || source.height > 8192 ||
        source.jitterWidth != source.width || source.jitterHeight != source.height ||
        (source.projectionFlags & ~uint8_t(4)) || !NormalOrZero(measuredDeltaMilliseconds) ||
        measuredDeltaMilliseconds <= 0 || !NormalOrZero(currentMotionScale[0]) || !NormalOrZero(currentMotionScale[1]))
        return false;
    Matrix view {}, inverseView {}, p {}, pd {};
    if (!Decode(source.nativeView, view) || !Decode(source.inverseNativeView, inverseView) ||
        !Decode(source.nativeProjection, p) || !Decode(source.depthProjection, pd) ||
        !Perspective(p) || !RigidInverse(view, inverseView)) return false;

    Parameters result;
    result.renderSize = { float(source.width), float(source.height), 1.0f / float(source.width), 1.0f / float(source.height) };
    for (size_t i = 0; i < 2; ++i)
    {
        if (!NormalOrZero(source.lensOffset[i]) || !NormalOrZero(source.jitterPixels[i])) return false;
        // Match the native pixel-jitter NDC division before adding authored lens
        // offset. No subtract-nearly-equal reconstruction of that authored value.
        volatile float normalized = Float(source.jitterPixels[i]) / result.renderSize[i];
        volatile float ndc = float(normalized) * 2.0f;
        volatile float combined = Float(source.lensOffset[i]) + float(ndc);
        if (!NormalOrZero(float(ndc)) || !NormalOrZero(float(combined)) || !Equal(p[8 + i], float(combined))) return false;
        result.jitterNdc[i] = ndc; // Native Y sign: SL and the late bridge each negate it once.
    }
    const bool reversed = (source.projectionFlags & 4) != 0;
    Matrix expected = p;
    if (reversed)
    {
        expected[10] = 1.0f - p[10];
        expected[14] = -p[14];
    }
    for (size_t i = 0; i < pd.size(); ++i)
        if (!Equal(pd[i], expected[i])) return false;

    const auto planes = FSRD::GetViewPlanes(pd[10], pd[14], pd[11], reversed);
    if (!NormalOrZero(planes.nearPlane) || !NormalOrZero(planes.farPlane) || planes.nearPlane <= 0 ||
        planes.farPlane <= planes.nearPlane || planes.isRightHanded) return false;
    result.nearPlane = planes.nearPlane; result.farPlane = planes.farPlane;
    result.infinitePlanePolicy = planes.isInfinite;
    result.aspectRatio = pd[5] / pd[0];
    result.verticalFovRadians = 2.0f * std::atan(1.0f / pd[5]);
    if (!NormalOrZero(result.aspectRatio) || result.aspectRatio <= 0 || !NormalOrZero(result.verticalFovRadians) ||
        result.verticalFovRadians <= 0 || result.verticalFovRadians >= 3.141593f) return false;

    // Inverse of native-row [a,0,0,0; 0,b,0,0; ox,oy,A,1; 0,0,B,0].
    // Keep the exact stored A/B and actual authored offsets; no FOV/near/far refit.
    Matrix inverseProjection {};
    inverseProjection[0] = 1.0f / pd[0];
    inverseProjection[5] = 1.0f / pd[5];
    inverseProjection[11] = 1.0f / pd[14];
    inverseProjection[12] = -Float(source.lensOffset[0]) / pd[0];
    inverseProjection[13] = -Float(source.lensOffset[1]) / pd[5];
    inverseProjection[14] = 1.0f;
    inverseProjection[15] = -pd[10] / pd[14];
    for (size_t i = 0; i < inverseProjection.size(); ++i)
    {
        if (!NormalOrZero(inverseProjection[i])) return false;
        result.inverseProjection[i] = std::bit_cast<uint32_t>(inverseProjection[i]);
    }
    result.inverseView = source.inverseNativeView;
    result.previousView = source.nativeView;
    result.previousDepthProjection = { pd[10], pd[14], pd[11], 0 };
    result.cameraRight = UnitRow(inverseView, 0);
    result.cameraUp = UnitRow(inverseView, 1);
    result.cameraForward = UnitRow(inverseView, 2); // Admitted canonical W=+1, no sample's -Z flip.
    result.cameraPositionDelta = { 0, 0, 0 };
    result.motionScale = { currentMotionScale[0], currentMotionScale[1], 1 };
    result.deltaMilliseconds = measuredDeltaMilliseconds;
    result.frameIndex = frameIndex; // Zero is a valid observed counter, not missing metadata.
    result.conversionFlags = ConversionNonGammaPackedReset;
    result.dispatchFlags = DispatchResetNonGamma;
    output = result;
    return true;
}

#undef FSRD_RESET_CAMERA_PRECISE
#if defined(_MSC_VER) || defined(__clang__)
#pragma float_control(pop)
#endif
} // namespace FSRD::CyberpunkResetCamera
