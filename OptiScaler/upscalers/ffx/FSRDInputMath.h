#pragma once
#include "FSRDDepthMotion.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace FSRD
{
struct ViewPlanes
{
    float nearPlane;
    float farPlane;
    bool isInfinite;
    bool isRightHanded;
};

// Column-vector projection: clip.z = A * view.z + B; clip.w = W * view.z.
// Handedness depends on W, not B (whose sign also changes with reversed depth).
inline ViewPlanes GetViewPlanes(float a, float b, float w, bool inverted)
{
    const bool infinite = std::abs(inverted ? a : a - w) < 1e-6f;
    const float nearPlane = std::abs(inverted ? b / (w - a) : -b / a);
    const float farPlane = infinite ? std::numeric_limits<float>::max()
                                    : std::abs(inverted ? -b / a : b / (w - a));
    return { nearPlane, farPlane, infinite, w < 0.0f };
}

constexpr bool FitsRenderSize(uint32_t width, uint32_t height, uint32_t maxWidth, uint32_t maxHeight)
{
    return width > 0 && height > 0 && width <= maxWidth && height <= maxHeight;
}
}
