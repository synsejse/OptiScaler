#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace FSRD::CyberpunkMotionScale
{
// Exact authenticated Cyberpunk image only. These are configurable current SL
// normalization multipliers, not defaults and not pixel-space NGX multipliers.
inline constexpr std::array<uintptr_t, 2> PropertyRvas { 0x3464d90, 0x3464de0 };
inline constexpr std::array<uintptr_t, 2> NameRvas { 0x300c8f0, 0x300c8e0 };
inline constexpr uintptr_t SectionRva = 0x300c86c, VtableRva = 0x2c1dd58;
struct Snapshot
{
    std::array<uint32_t, 2> words {};
    bool operator==(const Snapshot&) const = default;
};

namespace Detail
{
template<class Host, class T> bool Read(Host& host, uintptr_t address, T& output)
{
    return address && sizeof(T) - 1 <= std::numeric_limits<uintptr_t>::max() - address &&
           host.Read(address, &output, sizeof(output));
}
template<class Host> bool Sample(Host& host, uintptr_t image, Snapshot& value)
{
    constexpr std::array<char, 5> Section { 'D', 'L', 'S', 'S', 0 };
    constexpr std::array<std::array<char, 11>, 2> Names {{
        { 'M', 'v', 'e', 'c', 'S', 'c', 'a', 'l', 'e', 'X', 0 },
        { 'M', 'v', 'e', 'c', 'S', 'c', 'a', 'l', 'e', 'Y', 0 } }};
    std::array<char, 5> section {};
    if (!Read(host, image + SectionRva, section) || section != Section) return false;
    for (std::size_t i = 0; i < PropertyRvas.size(); ++i)
    {
        const auto property = image + PropertyRvas[i];
        std::array<uintptr_t, 3> identity {};
        std::array<char, 11> name {};
        if (!Read(host, property, identity) || identity[0] != image + VtableRva ||
            identity[1] != image + NameRvas[i] || identity[2] != image + SectionRva ||
            !Read(host, identity[1], name) || name != Names[i] ||
            !Read(host, property + 0x30, value.words[i])) return false;
        // Safe under /fp:fast, and preserve every finite original bit, including -0.
        if ((value.words[i] & 0x7f800000u) == 0x7f800000u) return false;
    }
    return true;
}
}

// Bounded CPU reads only; caller authenticates the full installed image. Repeated
// equality detects some mutation, not atomicity, GPU use or same-frame provenance.
// Does not retain pointers, call property getters, apply scaling or select defaults.
template<class Host> bool Observe(Host& host, uintptr_t image, Snapshot& output) noexcept
{
    try
    {
        constexpr uintptr_t ImageBytes = 0x04efc000;
        if (!image || image > std::numeric_limits<uintptr_t>::max() - ImageBytes) return false;
        Snapshot first, second;
        if (!Detail::Sample(host, image, first) || !Detail::Sample(host, image, second) || first != second)
            return false;
        output = first;
        return true;
    }
    catch (...) { return false; }
}
}
