#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace FSRD::CyberpunkLightingSource
{
// Narrow no-call reader for the authenticated154610 final-lighting t8 owner
// route. Actual binder receipt and pixel consumption are SEPARATE caller proof.
// No sampled pointer is dereferenced as COM, and no signal semantics are guessed.
inline constexpr uintptr_t BinderRva = 0x1f3a6c;
inline constexpr uintptr_t BindSixReturnRva = 0x155cc0, BindFiveReturnRva = 0x155d24;
struct Snapshot
{
    uintptr_t context = 0, view = 0, owner = 0, registry = 0, slot = 0;
    uint64_t viewFlags = 0;
    uint32_t handle = 0, requestedState = 0;
    int32_t refs = 0;
    uintptr_t native = 0, descriptor = 0, externalSync = 0;
    std::array<uint8_t, 12> compact {};
    bool operator==(const Snapshot&) const = default;
};

template <typename Host, typename T>
bool Read(Host& host, uintptr_t base, uintptr_t offset, T& result)
{
    static_assert(std::is_trivially_copyable_v<T>);
    constexpr auto Max = std::numeric_limits<uintptr_t>::max();
    return base && offset <= Max - base && sizeof(T) - 1 <= Max - (base + offset) &&
        host.Read(base + offset, &result, sizeof(T));
}

inline bool IsFinalBind(uintptr_t image, uintptr_t caller, uint32_t first, uint32_t count, uint8_t stage)
{
    return image && image <= UINTPTR_MAX - BindFiveReturnRva && first == 5 && stage == 1 &&
        ((caller == image + BindSixReturnRva && count == 6) ||
         (caller == image + BindFiveReturnRva && count == 5));
}

// Caller authenticates original lighting/binder/factory bodies (LightingCode),
// current executing scope and shader, then repeats exact Snapshot equality.
// Only native owner+26c is supported: no graph/alternate/default fallback.
template <typename Host>
bool Observe(Host& host, uintptr_t image, uintptr_t context, Snapshot& output) noexcept
{
    output = {};
    try
    {
        Snapshot s; s.context = context;
        uint8_t executing = 0;
        if (!image || image > UINTPTR_MAX - 0x04efc000 ||
            !Read(host, context, 0x30, executing) || !(executing & 2) ||
            !Read(host, context, 0x18, s.view) || !s.view ||
            !Read(host, s.view, 0x17d0, s.viewFlags) || !(s.viewFlags & (uint64_t(1) << 51)) ||
            (s.viewFlags & (uint64_t(1) << 55)) ||
            !Read(host, s.view, 0x1d70, s.owner) || !s.owner ||
            !Read(host, s.owner, 0x26c, s.handle) || !s.handle || s.handle > 0x8000 ||
            !Read(host, image, 0x3438a28, s.registry) || !s.registry)
            return false;
        const uintptr_t offset = 0x2f1d8 + uintptr_t(s.handle - 1) * 0xb0;
        if (offset > UINTPTR_MAX - s.registry) return false;
        s.slot = s.registry + offset;
        if (!Read(host, s.slot - 8, 0, s.refs) || s.refs <= 0 ||
            !Read(host, s.slot, 0, s.native) || !s.native ||
            !Read(host, s.slot, 0x30, s.descriptor) || !s.descriptor ||
            !Read(host, s.slot, 0x48, s.requestedState) || s.requestedState != 0xc0 ||
            !Read(host, s.slot, 0x4e, s.compact) ||
            !Read(host, s.slot, 0x68, s.externalSync) || s.externalSync)
            return false;
        // Authenticated ordinary Texture2D/mip0/plane0 SRV factory. Tag11
        // resolves to RGBA16_FLOAT; host still checks actual GetDesc/heap.
        if (s.compact[4] != 1 || s.compact[5] != 0 || (s.compact[6] & 0xf) != 0 ||
            s.compact[7] != 0x11 || !(s.compact[8] & 1) || (s.compact[8] & 4) ||
            (s.compact[8] & 0x40))
            return false;
        output = s;
        return true;
    }
    catch (...) { return false; }
}
} // namespace FSRD::CyberpunkLightingSource
