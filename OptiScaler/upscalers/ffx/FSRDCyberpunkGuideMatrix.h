#pragma once

#include "FSRDCyberpunkGuideConstants.h"

#include <cfenv>
#include <limits>
#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace FSRD::CyberpunkGuideMatrix
{
using MatrixWords = std::array<uint32_t, 16>; // Consecutive native rows, not transposed.
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<float>::digits == 24 && std::numeric_limits<double>::digits >= 53);

// Authenticated producer RVA785701 -> 8023ac: inverse jittered native projection
// times inverse native view with its entire fourth row replaced by [0,0,0,1].
// This pure helper obtains no engine inputs and establishes no frame/GPU provenance.
// The caller must supply authenticated, current-view source words and dimensions.
// Nearest-even is the validated arithmetic contract; do not change the caller's
// rounding, DAZ or FTZ modes to manufacture an apparently matching result.
#if defined(_MSC_VER) || defined(__clang__)
#pragma float_control(precise, on, push)
#endif
#if defined(_MSC_VER)
#define FSRD_GUIDE_MATRIX_PRECISE __declspec(noinline)
#elif defined(__GNUC__) && !defined(__clang__)
#define FSRD_GUIDE_MATRIX_PRECISE __attribute__((noinline, optimize("no-fast-math", "fp-contract=off")))
#elif defined(__clang__)
#define FSRD_GUIDE_MATRIX_PRECISE __attribute__((noinline))
#else
#define FSRD_GUIDE_MATRIX_PRECISE
#endif

namespace Detail
{
inline bool NormalOrZero(uint32_t word) noexcept
{
    const uint32_t exponent = word & 0x7f800000u;
    return exponent != 0x7f800000u && (exponent != 0 || (word & 0x007fffffu) == 0);
}

inline bool NearestRounding() noexcept
{
    if (std::fegetround() != FE_TONEAREST)
        return false;
#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__)
    // Also check SSE directly: its control word can differ from the x87 mode.
    if ((_mm_getcsr() & 0x6000u) != 0)
        return false;
#endif
    return true;
}

FSRD_GUIDE_MATRIX_PRECISE inline bool WideResultInRange(double value) noexcept
{
    // Finite normal binary32 operands cannot overflow/underflow binary64 here.
    // Checking BEFORE binary32 arithmetic catches a tiny nonzero intermediate
    // even when the executing thread has FTZ enabled and would flush it to zero.
    const double magnitude = value < 0.0 ? -value : value;
    return magnitude == 0.0 || (magnitude >= static_cast<double>((std::numeric_limits<float>::min)()) &&
                                magnitude <= static_cast<double>((std::numeric_limits<float>::max)()));
}

FSRD_GUIDE_MATRIX_PRECISE inline bool Multiply(float left, float right, float& output) noexcept
{
    if (!WideResultInRange(static_cast<double>(left) * static_cast<double>(right)))
        return false;
    volatile float rounded = left * right; // Separate binary32 rounding; never contract into an add.
    const float result = rounded;
    if (!NormalOrZero(std::bit_cast<uint32_t>(result)))
        return false;
    output = result;
    return true;
}

FSRD_GUIDE_MATRIX_PRECISE inline bool Add(float left, float right, float& output) noexcept
{
    if (!WideResultInRange(static_cast<double>(left) + static_cast<double>(right)))
        return false;
    volatile float rounded = left + right;
    const float result = rounded;
    if (!NormalOrZero(std::bit_cast<uint32_t>(result)))
        return false;
    output = result;
    return true;
}
} // namespace Detail

FSRD_GUIDE_MATRIX_PRECISE inline bool Generate(const MatrixWords& inverseProjection, const MatrixWords& inverseView,
                                              uint32_t width, uint32_t height,
                                              CyberpunkGuideConstants::ObservedSharedWords& output) noexcept
{
    if (!width || !height || !Detail::NearestRounding())
        return false;
    for (uint32_t i = 0; i < 16; ++i)
        if (!Detail::NormalOrZero(inverseProjection[i]) || !Detail::NormalOrZero(inverseView[i]))
            return false; // Validate even the supplied row which the authored producer replaces.

    MatrixWords rotation = inverseView;
    rotation[12] = rotation[13] = rotation[14] = 0;
    rotation[15] = 0x3f800000u;
    CyberpunkGuideConstants::ObservedSharedWords generated {};
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t column = 0; column < 4; ++column)
        {
            float products[4] {};
            for (uint32_t k = 0; k < 4; ++k)
                if (!Detail::Multiply(std::bit_cast<float>(inverseProjection[row * 4 + k]),
                                      std::bit_cast<float>(rotation[k * 4 + column]), products[k]))
                    return false;
            float sum = 0;
            // Exact authored SSE order: ((p1 + p0) + p2) + p3, not a generic dot product.
            if (!Detail::Add(products[1], products[0], sum) || !Detail::Add(sum, products[2], sum) ||
                !Detail::Add(sum, products[3], sum))
                return false;
            generated[row][column] = std::bit_cast<uint32_t>(sum);
        }

    // RVA78580f..78584c converts zero-extended uint32 dimensions to binary32,
    // then uses separate DIVSS(1.0f, dimension), not reciprocal approximation.
    volatile float activeWidth = static_cast<float>(width), activeHeight = static_cast<float>(height);
    const float w = activeWidth, h = activeHeight;
    volatile float inverseWidth = 1.0f / w, inverseHeight = 1.0f / h;
    generated[4] = { std::bit_cast<uint32_t>(w), std::bit_cast<uint32_t>(h),
                     std::bit_cast<uint32_t>(static_cast<float>(inverseWidth)),
                     std::bit_cast<uint32_t>(static_cast<float>(inverseHeight)) };
    for (uint32_t word : generated[4])
        if (!Detail::NormalOrZero(word))
            return false;

    output = generated; // Every rejection leaves the caller's entire payload unchanged.
    return true;
}

#undef FSRD_GUIDE_MATRIX_PRECISE
#if defined(_MSC_VER) || defined(__clang__)
#pragma float_control(pop)
#endif
} // namespace FSRD::CyberpunkGuideMatrix
