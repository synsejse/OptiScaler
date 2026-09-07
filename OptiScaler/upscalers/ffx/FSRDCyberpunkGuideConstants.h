#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace FSRD::CyberpunkGuideConstants
{
// Pure packing for the authenticated m_dlssConvertData shader. No engine memory,
// GPU work, camera reconstruction, frame association or resource-readiness policy.
// Caller must authenticate the shader and provenance of every supplied source.
inline constexpr uint32_t SharedRegisters[] = { 21, 22, 23, 24, 27 };
using ObservedSharedWords = std::array<std::array<uint32_t, 4>, 5>;
using SharedConstants = std::array<uint32_t, 1584 / sizeof(uint32_t)>;
using PassConstants = std::array<uint32_t, 8>;
static_assert(sizeof(ObservedSharedWords) == 80 && sizeof(SharedConstants) == 1584);
static_assert(sizeof(PassConstants) == 32 && std::endian::native == std::endian::little);

// The authenticated guide shader reads ONLY these five cb12 registers. Preserve
// their native words, including signed zero; never transpose or recompute here.
// Zeros in unread registers are not a replacement for the full engine shared CB.
inline SharedConstants PackShared(const ObservedSharedWords& source) noexcept
{
    SharedConstants result {};
    for (uint32_t i = 0; i < 5; ++i)
        for (uint32_t component = 0; component < 4; ++component)
            result[SharedRegisters[i] * 4 + component] = source[i][component];
    return result;
}

enum class TransparencyInput : uint8_t
{
    Unspecified,
    PreTransparencySurface, // Deliberately omit a later transparent layer; not identical to late DLSSD guides.
    AuthoredGuide           // Caller owns the actual current t3/t6 resources and their validated views.
};

struct PassSources
{
    uint32_t width = 0;  // Authored guide OUTPUT descriptor dimensions, not assumed display/view dimensions.
    uint32_t height = 0;
    TransparencyInput transparency = TransparencyInput::Unspecified;
    int32_t noVMode = 0; // Preserve the exact authored integer; do not turn it into a bool.
    uint8_t extraSpecularEnabled = 0; // Exact zero-extended authored byte (not normalized to 0/1).
    uint32_t extraSpecularScaleBits = 0; // Exact float word; disabled-view source must already supply zero.
};

// Original producer RVA37deac..37df3e converts uint16 dimensions and uses scalar
// DIVSS(1.0f, dimension). Keep this small operation precise despite project /fp:fast.
// Like the game, arithmetic uses the executing thread's floating-point environment.
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#elif defined(__clang__)
#pragma float_control(precise, on, push)
#endif
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-fast-math")))
#endif
inline bool PackPass(const PassSources& source, PassConstants& output) noexcept
{
    if (!source.width || !source.height || source.width > 65535 || source.height > 65535 ||
        (source.transparency != TransparencyInput::PreTransparencySurface &&
         source.transparency != TransparencyInput::AuthoredGuide) ||
        (source.extraSpecularScaleBits & 0x7f800000u) == 0x7f800000u)
        return false;

    const float width = static_cast<float>(source.width), height = static_cast<float>(source.height);
    const float inverseWidth = 1.0f / width, inverseHeight = 1.0f / height;
    const PassConstants packed = {
        std::bit_cast<uint32_t>(width), std::bit_cast<uint32_t>(height),
        std::bit_cast<uint32_t>(inverseWidth), std::bit_cast<uint32_t>(inverseHeight),
        source.transparency == TransparencyInput::AuthoredGuide ? 1u : 0u,
        std::bit_cast<uint32_t>(source.noVMode), static_cast<uint32_t>(source.extraSpecularEnabled),
        source.extraSpecularScaleBits
    };
    output = packed; // Failure never publishes a partial payload.
    return true;
}
#if defined(_MSC_VER) || defined(__clang__)
#pragma float_control(pop)
#endif
} // namespace FSRD::CyberpunkGuideConstants
