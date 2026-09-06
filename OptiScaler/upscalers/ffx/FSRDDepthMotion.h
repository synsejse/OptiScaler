#ifndef FSRD_DEPTH_MOTION_H
#define FSRD_DEPTH_MOTION_H

// Shared by C++ regression tests and the actual HLSL converter: no fitted encoding.
// Cyberpunk 2.31's camera and geometry velocity shaders write 1000 * HW-depth delta.
// See docs/FSR_RR_MOTION.md for the locally verified producer and applicability limits.
#ifdef __cplusplus
#include <cmath>
namespace FSRD
{
#define FSRD_MOTION_INLINE inline
#define FSRD_MOTION_FINITE std::isfinite
#define FSRD_MOTION_ABS std::abs
#define FSRD_MOTION_OUT_FLOAT float&
#else
#define FSRD_MOTION_INLINE
#define FSRD_MOTION_FINITE isfinite
#define FSRD_MOTION_ABS abs
#define FSRD_MOTION_OUT_FLOAT inout float
#endif

FSRD_MOTION_INLINE bool DecodeCyberpunkDepthMotion(float currentHardwareDepth, float encodedZ, float writerClass,
                                                  float currentLinearDepth, float previousA, float previousB,
                                                  float previousW, FSRD_MOTION_OUT_FLOAT depthDelta)
{
    if (!FSRD_MOTION_FINITE(currentHardwareDepth) || currentHardwareDepth < 0.0f || currentHardwareDepth > 1.0f ||
        !FSRD_MOTION_FINITE(encodedZ) || (writerClass != 0.0f && writerClass != 1.0f) ||
        !FSRD_MOTION_FINITE(currentLinearDepth) || currentLinearDepth <= 0.0f ||
        !FSRD_MOTION_FINITE(previousA) || !FSRD_MOTION_FINITE(previousB) || previousB == 0.0f ||
        FSRD_MOTION_ABS(previousW) != 1.0f)
        return false;

    const float previousHardwareDepth = currentHardwareDepth + encodedZ / 1000.0f;
    if (!FSRD_MOTION_FINITE(previousHardwareDepth) || previousHardwareDepth < 0.0f || previousHardwareDepth > 1.0f)
        return false;

    // Column-vector conventional perspective: clip.z=A*view.z+B, clip.w=W*view.z.
    // Historical depth must be decoded with historical projection coefficients.
    const float denominator = previousW * previousHardwareDepth - previousA;
    if (!FSRD_MOTION_FINITE(denominator) || denominator == 0.0f)
        return false;
    const float previousViewZ = previousB / denominator;
    if (!FSRD_MOTION_FINITE(previousViewZ) || previousViewZ * previousW <= 0.0f)
        return false;

    const float decodedDelta = FSRD_MOTION_ABS(previousViewZ) - currentLinearDepth;
    if (!FSRD_MOTION_FINITE(decodedDelta))
        return false;
    depthDelta = decodedDelta;
    return true;
}

#undef FSRD_MOTION_INLINE
#undef FSRD_MOTION_FINITE
#undef FSRD_MOTION_ABS
#undef FSRD_MOTION_OUT_FLOAT
#ifdef __cplusplus
} // namespace FSRD
#endif

#endif
