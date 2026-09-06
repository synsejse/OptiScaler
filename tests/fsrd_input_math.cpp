#include "../OptiScaler/upscalers/ffx/FSRDInputMath.h"
#include <cassert>
#include <iostream>

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
    std::cout << "FSRD projection and render-size tests passed\n";
}
