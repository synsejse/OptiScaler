#include "../OptiScaler/upscalers/ffx/FSRDInputMath.h"
#include <cassert>
#include <iostream>

namespace
{
struct Projection
{
    float a;
    float b;
    float w;
};

Projection MakeProjection(float w, bool reversed, bool infinite, float nearPlane, float farPlane)
{
    if (infinite)
        return { reversed ? 0.0f : w, reversed ? nearPlane : -nearPlane, w };
    return { reversed ? -w * nearPlane / (farPlane - nearPlane)
                      : w * farPlane / (farPlane - nearPlane),
             (reversed ? 1.0f : -1.0f) * farPlane * nearPlane / (farPlane - nearPlane), w };
}

// Independent forward projection, not a rearrangement of the decoder under test.
float HardwareDepth(const Projection& projection, float positiveLinearDepth)
{
    const double viewZ = static_cast<double>(projection.w) * positiveLinearDepth;
    const double clipZ = projection.a * viewZ + projection.b;
    const double clipW = projection.w * viewZ;
    return static_cast<float>(clipZ / clipW);
}

void CheckDepthMotion(const Projection& current, const Projection& previous, float currentLinearDepth,
                      float previousLinearDepth, float writerClass)
{
    const float currentHardwareDepth = HardwareDepth(current, currentLinearDepth);
    const float previousHardwareDepth = HardwareDepth(previous, previousLinearDepth);
    const float encodedZ = 1000.0f * (previousHardwareDepth - currentHardwareDepth);
    float result = -1234.0f;
    assert(FSRD::DecodeCyberpunkDepthMotion(currentHardwareDepth, encodedZ, writerClass, currentLinearDepth,
                                           previous.a, previous.b, previous.w, result));
    // The producer's FP32 subtraction/scaling loses some precision before decoding.
    const float tolerance = 1e-4f * (1.0f + currentLinearDepth + previousLinearDepth);
    assert(std::abs(result - (previousLinearDepth - currentLinearDepth)) < tolerance);
}

void TestCyberpunkDepthMotion()
{
    // Standard/reversed, left/right-handed, finite/infinite perspective projections.
    // A stationary camera with moving geometry must retain nonzero object depth motion.
    for (float w : { -1.0f, 1.0f })
        for (bool reversed : { false, true })
            for (bool infinite : { false, true })
                for (float writerClass : { 0.0f, 1.0f })
                {
                    const auto projection = MakeProjection(w, reversed, infinite, 0.1f, 100.0f);
                    CheckDepthMotion(projection, projection, 5.0f, 4.0f, writerClass);
                    CheckDepthMotion(projection, projection, 5.0f, 6.0f, writerClass);
                    CheckDepthMotion(projection, projection, 5.0f, 5.0f, writerClass);
                }

    // The previous projection is essential when camera clipping planes change.
    // Decoding previous hardware depth with current coefficients yields the wrong motion.
    for (float w : { -1.0f, 1.0f })
        for (bool reversed : { false, true })
        {
            const auto current = MakeProjection(w, reversed, false, 0.1f, 100.0f);
            const auto previous = MakeProjection(w, reversed, false, 0.5f, 250.0f);
            CheckDepthMotion(current, previous, 5.0f, 12.0f, 0.0f);
            CheckDepthMotion(current, previous, 5.0f, 12.0f, 1.0f);
            const float currentHardwareDepth = HardwareDepth(current, 5.0f);
            const float encodedZ = 1000.0f * (HardwareDepth(previous, 12.0f) - currentHardwareDepth);
            float wrongProjectionResult = 0.0f;
            assert(FSRD::DecodeCyberpunkDepthMotion(currentHardwareDepth, encodedZ, 0.0f, 5.0f,
                                                   current.a, current.b, current.w, wrongProjectionResult));
            assert(std::abs(wrongProjectionResult - 7.0f) > 1.0f);
        }

    // Endpoints 0 and 1 are valid for finite projections; these coefficients are exact.
    for (float w : { -1.0f, 1.0f })
        for (bool reversed : { false, true })
        {
            const auto projection = MakeProjection(w, reversed, false, 1.0f, 2.0f);
            CheckDepthMotion(projection, projection, 1.0f, 2.0f, 0.0f);
            CheckDepthMotion(projection, projection, 2.0f, 1.0f, 1.0f);
        }

    // The helper computes physical delta; FP16 bounding belongs to the shader caller.
    const auto largeDepthProjection = MakeProjection(1.0f, true, true, 1.0f, 0.0f);
    const float currentLargeHardware = HardwareDepth(largeDepthProjection, 50000.0f);
    const float largeEncoded = 1000.0f * (HardwareDepth(largeDepthProjection, 200000.0f) - currentLargeHardware);
    float largeDelta = 0.0f;
    assert(FSRD::DecodeCyberpunkDepthMotion(currentLargeHardware, largeEncoded, 0.0f, 50000.0f,
                                           0.0f, 1.0f, 1.0f, largeDelta));
    assert(largeDelta > 100000.0f);

    const auto reject = [](float hardware, float encoded, float writer, float linear, float a, float b, float w)
    {
        constexpr float sentinel = 1234.0f;
        float result = sentinel;
        assert(!FSRD::DecodeCyberpunkDepthMotion(hardware, encoded, writer, linear, a, b, w, result));
        assert(result == sentinel);
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (float invalid : { nan, infinity, -infinity })
    {
        reject(invalid, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f);
        reject(0.5f, invalid, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f);
        reject(0.5f, 0.0f, invalid, 2.0f, 0.0f, 1.0f, 1.0f);
        reject(0.5f, 0.0f, 0.0f, invalid, 0.0f, 1.0f, 1.0f);
        reject(0.5f, 0.0f, 0.0f, 2.0f, invalid, 1.0f, 1.0f);
        reject(0.5f, 0.0f, 0.0f, 2.0f, 0.0f, invalid, 1.0f);
        reject(0.5f, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, invalid);
    }
    reject(-0.01f, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f);
    reject(1.01f, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f);
    reject(0.5f, -501.0f, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f); // Previous depth < 0.
    reject(0.5f, +501.0f, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f); // Previous depth > 1.
    for (float writer : { -1.0f, 0.5f, 2.0f })
        reject(0.5f, 0.0f, writer, 2.0f, 0.0f, 1.0f, 1.0f);
    reject(0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    reject(0.5f, 0.0f, 0.0f, -2.0f, 0.0f, 1.0f, 1.0f);
    reject(0.5f, 0.0f, 0.0f, 2.0f, 0.5f, 1.0f, 1.0f); // Zero denominator.
    reject(0.5f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 1.0f); // Degenerate B.
    reject(0.5f, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, 0.0f);
    reject(0.5f, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, 2.0f);
    reject(0.5f, 0.0f, 0.0f, 2.0f, 0.0f, -1.0f, 1.0f); // Behind LH camera.
    reject(0.5f, 0.0f, 0.0f, 2.0f, 0.0f, -1.0f, -1.0f); // Behind RH camera.
    reject(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f); // Reversed infinite far plane.
    reject(1.0f, 0.0f, 0.0f, 2.0f, 1.0f, -1.0f, 1.0f); // Standard infinite far plane.
    reject(0.5f, 0.0f, 0.0f, 2.0f, 0.0f, std::numeric_limits<float>::max(), 1.0f); // Overflow.
}
}

int main()
{
    // Exercise the production projection helper for positive/negative Z and both depth directions.
    constexpr float nearPlane = 0.1f, farPlane = 100.0f;
    for (float w : { -1.0f, 1.0f })
        for (bool inverted : { false, true })
        {
            const float a = inverted ? -w * nearPlane / (farPlane - nearPlane)
                                     : w * farPlane / (farPlane - nearPlane);
            const float b = (inverted ? 1.0f : -1.0f) * farPlane * nearPlane / (farPlane - nearPlane);
            const auto planes = FSRD::GetViewPlanes(a, b, w, inverted);
            assert(std::abs(planes.nearPlane - nearPlane) < 1e-5f);
            assert(std::abs(planes.farPlane - farPlane) < 0.01f);
            assert(planes.isRightHanded == (w < 0.0f));
            assert(!planes.isInfinite);

            const auto infinite = FSRD::GetViewPlanes(inverted ? 0.0f : w,
                                                      inverted ? nearPlane : -nearPlane, w, inverted);
            assert(infinite.isInfinite);
            assert(std::isfinite(infinite.farPlane));
            assert(std::abs(infinite.nearPlane - nearPlane) < 1e-5f);
            assert(infinite.isRightHanded == (w < 0.0f));
        }

    // Quality changes, non-multiple-of-8 dimensions, native resolution and invalid sizes.
    for (uint32_t width : { 1280u, 1485u, 1707u, 2560u })
        assert(FSRD::FitsRenderSize(width, 1440, 2560, 1440));
    assert(!FSRD::FitsRenderSize(0, 720, 2560, 1440));
    assert(!FSRD::FitsRenderSize(1280, 0, 2560, 1440));
    assert(!FSRD::FitsRenderSize(2561, 1440, 2560, 1440));
    assert(!FSRD::FitsRenderSize(2560, 1441, 2560, 1440));
    TestCyberpunkDepthMotion();
    std::cout << "FSRD projection, depth-motion and render-size tests passed\n";
}
